"use strict";
// Browser side of the master's web GUI (ported from Assignment 3's beatbox_ui.js).
// Polls the master over the websocket and draws the node triangle, each node's
// direction line for the latest sound, a heat map of where the lines agree,
// and the master's estimate. No animation: the map is redrawn on every status.
//
// Map convention (master/include/layout.h): metres, origin at the triangle's
// centre, x right, y up, angles in degrees counter-clockwise from +x. The
// master already converts each node's theta to a map bearing.

/* ---------- tuning ---------- */
var STATUS_POLL_MS = 100;
var UPTIME_POLL_MS = 1000;
var SERVER_TIMEOUT_MS = 1000;
var ERROR_SHOW_MS = 5000;
var STALE_NODE_MS = 3000;       // matches PROTO_TIMEOUT_MS

var MAP_PADDING_M = 0.9;        // space around the triangle
var GRID_STEP_M = 0.5;
var RAY_PAST_FIX_M = 0.4;       // lines stop this far past the estimate
var TRAIL_LENGTH = 7;           // earlier estimates shown as dots

var HEAT_CELLS = 150;           // heat map resolution (cells per side)
var HEAT_SIGMA_DEG = 5;         // angular width of a line at confidence 1
var HEAT_MIN_CONFIDENCE = 0.3;
var HEAT_NODE_CLEARANCE_M = 0.06;

// Quality rating. PLACEHOLDERS: set these from real tap tests.
var GOOD_SPREAD_M = 0.05;
var FAIR_SPREAD_M = 0.15;
var LOW_CONFIDENCE = 0.5;

/* ---------- state ---------- */
var socket = io.connect();
var communicationsTimeout = null;
var hideErrorTimeout = null;

var nodes = [];                 // latest node list from the master
var nodesById = {};
var fixes = [];                 // newest first
var view = null;                // metres <-> pixels
var heatCanvas = document.createElement('canvas');
var heatKey = "";               // which fix and view the heat map was built for
var colors = {};

document.addEventListener('DOMContentLoaded', function() {
	readColors();
	setupServerMessageHandlers();

	sendCommandToServer('status');
	sendCommandToServer('read-uptime');
	window.setInterval(function() { sendCommandToServer('status'); }, STATUS_POLL_MS);
	window.setInterval(function() { sendCommandToServer('read-uptime'); }, UPTIME_POLL_MS);

	document.getElementById('simulate').addEventListener('click', function() {
		sendCommandToServer('sound');
	});
	document.getElementById('stop').addEventListener('click', function() {
		if (window.confirm("Stop the master program?")) {
			sendCommandToServer('stop');
		}
	});
	window.addEventListener('resize', drawMap);
	window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function() {
		readColors();
		heatKey = "";
		drawMap();
	});
});

function readColors() {
	var css = getComputedStyle(document.documentElement);
	['map-bg', 'map-grid', 'map-label', 'map-triangle', 'node-1', 'node-2', 'node-3',
	 'fix', 'heat-1', 'heat-2'].forEach(function(name) {
		colors[name] = css.getPropertyValue('--' + name).trim();
	});
}

function nodeColor(id) {
	return colors['node-' + (((id - 1) % 3) + 1)];
}

/* ---------- messages from the server ---------- */

function setupServerMessageHandlers() {
	socket.on('status-reply', function(message) {
		clearServerTimeout();
		var status;
		try {
			status = JSON.parse(message);
		} catch (e) {
			errorHandler("Bad status from master: " + message);
			return;
		}
		setLinkState(true);
		onStatus(status);
	});

	socket.on('uptime-reply', function(message) {
		clearServerTimeout();
		var seconds = Number(message.split(" ")[0]);
		setText('board-uptime', formatDuration(seconds * 1000));
	});

	socket.on('sound-reply', clearServerTimeout);

	socket.on('stop-reply', function() {
		clearServerTimeout();
		setLinkState(false, "master stopped");
	});

	socket.on('master-error', errorHandler);
}

function onStatus(status) {
	document.getElementById('demo-badge').hidden = !status.demo;
	document.getElementById('demo-note').hidden = !status.demo;
	document.getElementById('simulate').hidden = !status.demo;
	setText('master-uptime', formatDuration(status.uptime_ms));

	nodes = status.nodes;
	nodesById = {};
	nodes.forEach(function(n) { nodesById[n.id] = n; });
	fixes = status.fixes;

	var latest = fixes.length > 0 ? fixes[0] : null;
	showFixPanel(latest);
	showNodeTable();
	showHistory();
	drawMap();
}

/* ---------- analysis of one fix ---------- */

// Per line: how far it passes from the estimate, and whether the estimate is behind its node
function analyseLines(fix) {
	return fix.bearings.map(function(b) {
		var node = nodesById[b.node];
		var result = {bearing: b, missM: null, isBehind: false};
		if (node && fix.position) {
			var angle = b.bearing * Math.PI / 180;
			var dx = fix.position.x - node.x;
			var dy = fix.position.y - node.y;
			result.missM = Math.abs(-dx * Math.sin(angle) + dy * Math.cos(angle));
			result.isBehind = (dx * Math.cos(angle) + dy * Math.sin(angle)) < 0;
		}
		return result;
	});
}

// Quality label and the reason for it (rules are explained on the page)
function rateFix(fix, lines) {
	if (!fix.position) {
		return {level: 'none', label: "No fix", reason: fix.bearings.length < 2
			? "Only one node heard it. At least two lines are needed to cross."
			: "The lines are (nearly) parallel, so they don't cross."};
	}
	var behind = lines.filter(function(l) { return l.isBehind; })
	                  .map(function(l) { return "N" + l.bearing.node; });
	if (behind.length > 0) {
		return {level: 'poor', label: "Poor", reason: "The lines cross behind " + behind.join(", ") +
			". A two-mic node can't tell front from back, so this is probably a mirror image."};
	}
	if (fix.bearings.length === 2) {
		return {level: 'fair', label: "Fair", reason:
			"Only two lines. Two lines always cross somewhere, so nothing checks this estimate."};
	}
	var spread = fix.position.rms;
	if (spread <= GOOD_SPREAD_M) {
		return {level: 'good', label: "Good", reason: "All three lines pass within " +
			cm(spread) + " (RMS) of the estimate."};
	}
	var worst = lines.reduce(function(a, b) { return (b.missM > a.missM) ? b : a; });
	var level = (spread <= FAIR_SPREAD_M) ? 'fair' : 'poor';
	return {level: level, label: level === 'fair' ? "Fair" : "Poor", reason:
		"The lines disagree by " + cm(spread) + " (RMS). N" + worst.bearing.node +
		"'s line misses the most (" + cm(worst.missM) + ")."};
}

/* ---------- side panels ---------- */

function showFixPanel(fix) {
	var rows = document.getElementById('line-rows');
	if (!fix) {
		return;
	}
	var lines = analyseLines(fix);
	var rating = rateFix(fix, lines);

	setText('fix-title', "Sound #" + fix.id);
	setText('fix-age', formatAge(fix.age_ms));
	setText('fix-position', fix.position
		? "(" + fix.position.x.toFixed(2) + ", " + fix.position.y.toFixed(2) + ") m"
		: "no position");
	var badge = document.getElementById('fix-quality');
	badge.textContent = rating.label;
	badge.className = "quality q-" + rating.level;
	badge.hidden = false;
	setText('fix-reason', rating.reason);

	setText('fix-spread', fix.position && fix.bearings.length > 2 ? cm(fix.position.rms) : "–");
	var total = fix.bearings.reduce(function(sum, b) { return sum + b.confidence; }, 0);
	setText('fix-confidence', fix.bearings.length > 0 ? (total / fix.bearings.length).toFixed(2) : "–");

	rows.innerHTML = "";
	lines.forEach(function(l) {
		var row = document.createElement('tr');
		addNodeCell(row, l.bearing.node);
		addCell(row, signedDeg(l.bearing.theta));
		addCell(row, l.bearing.bearing.toFixed(1) + "°");
		addCell(row, l.bearing.confidence.toFixed(2),
			l.bearing.confidence < LOW_CONFIDENCE ? 'bad' : null);
		addCell(row, l.missM === null ? "–" : cm(l.missM),
			l.isBehind ? 'bad' : null);
		rows.appendChild(row);
	});
	var heard = {};
	fix.bearings.forEach(function(b) { heard[b.node] = true; });
	nodes.forEach(function(n) {
		if (!heard[n.id]) {
			var row = document.createElement('tr');
			row.className = 'muted';
			addNodeCell(row, n.id);
			var cell = addCell(row, "didn't report this sound");
			cell.colSpan = 4;
			rows.appendChild(row);
		}
	});
}

function showNodeTable() {
	var rows = document.getElementById('node-rows');
	rows.innerHTML = "";
	if (nodes.length === 0) {
		rows.innerHTML = '<tr><td colspan="4">No nodes yet</td></tr>';
		return;
	}
	nodes.forEach(function(node) {
		var isLive = isNodeLive(node);
		var row = document.createElement('tr');
		addNodeCell(row, node.id);
		addCell(row, isLive ? "up" : "down", isLive ? 'ok' : 'bad');
		addCell(row, formatAge(node.seen_ms));
		addCell(row, String(node.events));
		rows.appendChild(row);
	});
}

function showHistory() {
	var list = document.getElementById('history');
	list.innerHTML = "";
	if (fixes.length === 0) {
		list.innerHTML = '<li class="muted">None yet</li>';
		return;
	}
	fixes.forEach(function(fix) {
		var rating = rateFix(fix, analyseLines(fix));
		var item = document.createElement('li');
		var what = document.createElement('span');
		var dot = document.createElement('span');
		dot.className = "qdot q-" + rating.level;
		dot.title = rating.label;
		what.appendChild(dot);
		what.appendChild(document.createTextNode("#" + fix.id + "  " + (fix.position
			? "(" + fix.position.x.toFixed(2) + ", " + fix.position.y.toFixed(2) + ")"
			: "no fix") + "  · " + fix.bearings.length + " line" + (fix.bearings.length === 1 ? "" : "s")));
		var when = document.createElement('span');
		when.className = 'when';
		when.textContent = formatAge(fix.age_ms);
		item.appendChild(what);
		item.appendChild(when);
		list.appendChild(item);
	});
}

function addCell(row, text, className) {
	var cell = document.createElement('td');
	cell.textContent = text;
	if (className) {
		cell.className = className;
	}
	row.appendChild(cell);
	return cell;
}

function addNodeCell(row, id) {
	var cell = document.createElement('td');
	var swatch = document.createElement('span');
	swatch.className = 'swatch';
	swatch.style.background = nodeColor(id);
	cell.appendChild(swatch);
	cell.appendChild(document.createTextNode("N" + id));
	row.appendChild(cell);
}

function isNodeLive(node) {
	return node.connected && node.seen_ms >= 0 && node.seen_ms < STALE_NODE_MS;
}

/* ---------- map ---------- */

function drawMap() {
	var canvas = document.getElementById('map');
	var ctx = canvas.getContext('2d');
	var ratio = window.devicePixelRatio || 1;
	var size = canvas.clientWidth;
	if (size === 0) {
		return;
	}
	if (canvas.width !== Math.round(size * ratio)) {
		canvas.width = Math.round(size * ratio);
		canvas.height = Math.round(size * ratio);
	}
	ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
	ctx.fillStyle = colors['map-bg'];
	ctx.fillRect(0, 0, size, size);

	view = makeView(size);
	drawGrid(ctx, size);
	drawTriangle(ctx);
	drawTrail(ctx);

	var fix = fixes.length > 0 ? fixes[0] : null;
	if (fix) {
		drawHeat(ctx, fix);
		fix.bearings.forEach(function(b) {
			var node = nodesById[b.node];
			if (node) {
				drawRay(ctx, node, b.bearing, rayLength(node, b.bearing, fix), nodeColor(node.id));
			}
		});
		if (fix.position) {
			drawEstimate(ctx, fix);
		}
	}
	nodes.forEach(function(node) { drawNode(ctx, node); });
}

// Square view that fits the triangle with padding; fixed so the map doesn't jump
function makeView(size) {
	var xs = nodes.map(function(n) { return n.x; });
	var ys = nodes.map(function(n) { return n.y; });
	if (xs.length === 0) {
		xs = [-1, 1];
		ys = [-1, 1];
	}
	var minX = Math.min.apply(null, xs), maxX = Math.max.apply(null, xs);
	var minY = Math.min.apply(null, ys), maxY = Math.max.apply(null, ys);
	var span = Math.max(maxX - minX, maxY - minY) + 2 * MAP_PADDING_M;
	var left = (minX + maxX) / 2 - span / 2;
	var bottom = (minY + maxY) / 2 - span / 2;
	var scale = size / span;
	return {
		size: size, span: span, left: left, bottom: bottom, scale: scale,
		toCanvas: function(x, y) {   // y is up on the map, down on the canvas
			return {x: (x - left) * scale, y: size - (y - bottom) * scale};
		}
	};
}

function drawGrid(ctx, size) {
	ctx.lineWidth = 1;
	ctx.strokeStyle = colors['map-grid'];
	ctx.fillStyle = colors['map-label'];
	ctx.font = "11px system-ui, sans-serif";
	var first = Math.ceil(view.left / GRID_STEP_M) * GRID_STEP_M;
	for (var x = first; x <= view.left + view.span; x += GRID_STEP_M) {
		var px = Math.round(view.toCanvas(x, 0).x) + 0.5;
		line(ctx, px, 0, px, size);
		ctx.fillText(x.toFixed(1), px + 3, size - 5);
	}
	first = Math.ceil(view.bottom / GRID_STEP_M) * GRID_STEP_M;
	for (var y = first; y <= view.bottom + view.span; y += GRID_STEP_M) {
		var py = Math.round(view.toCanvas(0, y).y) + 0.5;
		line(ctx, 0, py, size, py);
		ctx.fillText(y.toFixed(1) + " m", 4, py - 4);
	}
}

function drawTriangle(ctx) {
	if (nodes.length < 3) {
		return;
	}
	ctx.beginPath();
	nodes.forEach(function(node, i) {
		var p = view.toCanvas(node.x, node.y);
		if (i === 0) {
			ctx.moveTo(p.x, p.y);
		} else {
			ctx.lineTo(p.x, p.y);
		}
	});
	ctx.closePath();
	ctx.fillStyle = 'rgba(255, 255, 255, 0.025)';
	ctx.fill();
	ctx.setLineDash([6, 6]);
	ctx.strokeStyle = colors['map-triangle'];
	ctx.lineWidth = 1.5;
	ctx.stroke();
	ctx.setLineDash([]);
}

function drawTrail(ctx) {
	var count = 0;
	for (var i = 1; i < fixes.length && count < TRAIL_LENGTH; i++) {
		if (!fixes[i].position) {
			continue;
		}
		var p = view.toCanvas(fixes[i].position.x, fixes[i].position.y);
		ctx.globalAlpha = 0.55 * (1 - count / TRAIL_LENGTH);
		ctx.fillStyle = colors.fix;
		ctx.beginPath();
		ctx.arc(p.x, p.y, 3.5, 0, 2 * Math.PI);
		ctx.fill();
		count++;
	}
	ctx.globalAlpha = 1;
}

// Long enough to pass the estimate a little; across the whole map if there is none
function rayLength(node, bearingDeg, fix) {
	if (!fix.position) {
		return view.span * 1.5;
	}
	var angle = bearingDeg * Math.PI / 180;
	var along = (fix.position.x - node.x) * Math.cos(angle) + (fix.position.y - node.y) * Math.sin(angle);
	return Math.max(along, 0) + RAY_PAST_FIX_M;
}

function drawRay(ctx, node, bearingDeg, length, color) {
	var angle = bearingDeg * Math.PI / 180;
	var start = view.toCanvas(node.x, node.y);
	var end = view.toCanvas(node.x + length * Math.cos(angle), node.y + length * Math.sin(angle));
	ctx.save();
	ctx.lineCap = 'round';
	ctx.strokeStyle = color;
	ctx.globalAlpha = 0.18;
	ctx.lineWidth = 9;
	line(ctx, start.x, start.y, end.x, end.y);
	ctx.globalAlpha = 0.95;
	ctx.lineWidth = 2;
	line(ctx, start.x, start.y, end.x, end.y);
	ctx.restore();
}

// Dot at the estimate; "?" if it is behind a node (front/back mix-up)
function drawEstimate(ctx, fix) {
	var p = view.toCanvas(fix.position.x, fix.position.y);
	ctx.fillStyle = colors.fix;
	ctx.strokeStyle = colors['map-bg'];
	ctx.lineWidth = 3;
	ctx.beginPath();
	ctx.arc(p.x, p.y, 6, 0, 2 * Math.PI);
	ctx.stroke();
	ctx.fill();
	if (!fix.position.in_front) {
		ctx.fillStyle = colors['heat-1'];
		ctx.font = "bold 13px system-ui, sans-serif";
		ctx.fillText("?", p.x + 10, p.y - 8);
	}
}

function drawNode(ctx, node) {
	var p = view.toCanvas(node.x, node.y);
	var heading = node.heading * Math.PI / 180;
	var color = isNodeLive(node) ? nodeColor(node.id) : colors['map-label'];

	// Mic pair: a short bar across the heading (canvas y is flipped)
	var across = {x: Math.sin(heading), y: Math.cos(heading)};
	ctx.strokeStyle = color;
	ctx.lineWidth = 3;
	ctx.lineCap = 'round';
	line(ctx, p.x - 15 * across.x, p.y - 15 * across.y, p.x + 15 * across.x, p.y + 15 * across.y);

	// Body and facing tick
	ctx.fillStyle = colors['map-bg'];
	ctx.lineWidth = 2;
	ctx.beginPath();
	ctx.arc(p.x, p.y, 11, 0, 2 * Math.PI);
	ctx.fill();
	ctx.stroke();
	line(ctx, p.x + 11 * Math.cos(heading), p.y - 11 * Math.sin(heading),
	          p.x + 20 * Math.cos(heading), p.y - 20 * Math.sin(heading));

	ctx.fillStyle = color;
	ctx.font = "bold 11px system-ui, sans-serif";
	ctx.textAlign = "center";
	ctx.textBaseline = "middle";
	ctx.fillText("N" + node.id, p.x, p.y + 0.5);
	ctx.textAlign = "start";
	ctx.textBaseline = "alphabetic";
}

/* ---------- heat map ---------- */

function drawHeat(ctx, fix) {
	var key = fix.id + "|" + view.size + "|" + view.left + "|" + view.bottom + "|" + view.span;
	if (key !== heatKey) {
		heatKey = key;
		buildHeat(fix);
	}
	ctx.save();
	ctx.globalCompositeOperation = 'lighter';
	ctx.imageSmoothingEnabled = true;
	ctx.drawImage(heatCanvas, 0, 0, view.size, view.size);
	ctx.restore();
}

// For every cell: how well do all the lines agree that the sound came from here?
// Each line counts as a Gaussian in angle, HEAT_SIGMA_DEG wide at confidence 1
// and wider when the node is less sure. Multiplying them leaves the region where
// every line points at once. Brightness is relative to the best cell.
function buildHeat(fix) {
	heatCanvas.width = HEAT_CELLS;
	heatCanvas.height = HEAT_CELLS;
	var hctx = heatCanvas.getContext('2d');
	var image = hctx.createImageData(HEAT_CELLS, HEAT_CELLS);

	var lines = [];
	fix.bearings.forEach(function(b) {
		var node = nodesById[b.node];
		if (node) {
			var sigma = HEAT_SIGMA_DEG / Math.max(b.confidence, HEAT_MIN_CONFIDENCE);
			lines.push({x: node.x, y: node.y, bearing: b.bearing, k: 1 / (2 * sigma * sigma)});
		}
	});
	if (lines.length === 0) {
		hctx.clearRect(0, 0, HEAT_CELLS, HEAT_CELLS);
		return;
	}

	var scores = new Float32Array(HEAT_CELLS * HEAT_CELLS);
	var best = -Infinity;
	var cellM = view.span / HEAT_CELLS;
	for (var row = 0; row < HEAT_CELLS; row++) {
		var y = view.bottom + view.span - (row + 0.5) * cellM;
		for (var col = 0; col < HEAT_CELLS; col++) {
			var x = view.left + (col + 0.5) * cellM;
			var score = 0;
			for (var i = 0; i < lines.length; i++) {
				var dx = x - lines[i].x, dy = y - lines[i].y;
				if (dx * dx + dy * dy < HEAT_NODE_CLEARANCE_M * HEAT_NODE_CLEARANCE_M) {
					score = -Infinity;
					break;
				}
				var miss = wrapDeg(Math.atan2(dy, dx) * 180 / Math.PI - lines[i].bearing);
				score -= miss * miss * lines[i].k;
			}
			scores[row * HEAT_CELLS + col] = score;
			if (score > best) {
				best = score;
			}
		}
	}

	var low = hexToRgb(colors['heat-1']);
	var high = hexToRgb(colors['heat-2']);
	for (var j = 0; j < scores.length; j++) {
		var v = Math.exp(scores[j] - best);     // 1 where the lines agree best
		var px = j * 4;
		image.data[px]     = low.r + (high.r - low.r) * v;
		image.data[px + 1] = low.g + (high.g - low.g) * v;
		image.data[px + 2] = low.b + (high.b - low.b) * v;
		image.data[px + 3] = 220 * Math.pow(v, 0.6);
	}
	hctx.putImageData(image, 0, 0);
}

/* ---------- helpers ---------- */

function line(ctx, x1, y1, x2, y2) {
	ctx.beginPath();
	ctx.moveTo(x1, y1);
	ctx.lineTo(x2, y2);
	ctx.stroke();
}

function wrapDeg(a) {
	return ((a + 180) % 360 + 360) % 360 - 180;
}

function hexToRgb(hex) {
	var n = parseInt(hex.replace('#', ''), 16);
	return {r: (n >> 16) & 255, g: (n >> 8) & 255, b: n & 255};
}

function cm(metres) {
	return (metres * 100).toFixed(1) + " cm";
}

function signedDeg(deg) {
	return (deg > 0 ? "+" : "") + deg.toFixed(1) + "°";
}

function setText(id, text) {
	document.getElementById(id).textContent = text;
}

function setLinkState(isOk, text) {
	var badge = document.getElementById('link-state');
	badge.textContent = text || (isOk ? "live" : "no master");
	badge.className = "badge " + (isOk ? "ok" : "bad");
}

function formatDuration(ms) {
	var total = Math.floor(ms / 1000);
	var h = Math.floor(total / 3600);
	var m = Math.floor((total / 60) % 60);
	var s = total % 60;
	return h + ":" + pad2(m) + ":" + pad2(s);
}

function formatAge(ms) {
	if (ms < 0) {
		return "never";
	}
	if (ms < 1000) {
		return "just now";
	}
	if (ms < 60000) {
		return (ms / 1000).toFixed(0) + " s ago";
	}
	return Math.floor(ms / 60000) + " min ago";
}

function pad2(n) {
	return (n < 10 ? "0" : "") + n;
}

function sendCommandToServer(command) {
	if (communicationsTimeout === null) {
		communicationsTimeout = setTimeout(errorHandler, SERVER_TIMEOUT_MS,
			"ERROR: Unable to communicate with the HTTP server. Is the Node.js server running?");
	}
	socket.emit(command);
}

function clearServerTimeout() {
	clearTimeout(communicationsTimeout);
	communicationsTimeout = null;
}

function errorHandler(message) {
	console.log("ERROR: " + message);
	setText('error-text', message);
	setLinkState(false);
	document.getElementById('error-box').hidden = false;
	window.clearTimeout(hideErrorTimeout);
	hideErrorTimeout = window.setTimeout(function() {
		document.getElementById('error-box').hidden = true;
	}, ERROR_SHOW_MS);
	clearServerTimeout();
}
