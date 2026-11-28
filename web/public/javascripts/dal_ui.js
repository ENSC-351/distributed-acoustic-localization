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
var CANVAS_FONT = "'IBM Plex Mono', ui-monospace, monospace";
var CANVAS_SANS = "Barlow, 'Segoe UI', sans-serif";
var DISPLAY_UTC_OFFSET_H = -7;  // clock shown at UTC-7, 24-hour, no daylight-time switch
var STALE_NODE_MS = 3000;       // matches PROTO_TIMEOUT_MS

var MAP_PADDING_M = 0.9;        // space around the triangle
var GRID_STEP_M = 0.1;
var GRID_MAJOR_EVERY = 5;          // every 5th line (0.5 m) is brighter and always labelled
var GRID_LABEL_MIN_PX = 34;        // label every line only if they are this far apart
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

var nodes = [];                 // latest node list from the master
var nodesById = {};
var fixes = [];                 // newest first
var view = null;                // metres <-> pixels
var mainHeat = {canvas: document.createElement('canvas'), key: ""};   // heat image for the main map
var openSounds = {};            // fix id -> {heat} for rows opened in Recent sounds
var historyKey = "";            // fix ids currently listed, to rebuild only on change
var colors = {};
var isDemo = false;
var lastAnnouncedId = null;      // screen readers hear each new sound once
var isPaused = false;            // map, last sound and history frozen
var pendingStatus = null;       // newest status received while paused
var clockOffsetMs = 0;          // server clock minus this browser's clock
var lastSoundAt = {};           // node id -> {events, at}: when it last heard a sound
var detectedAt = {};            // fix id -> browser time (ms) the sound was detected

document.addEventListener('DOMContentLoaded', function() {
	readColors();
	setupServerMessageHandlers();

	sendCommandToServer('status');
	sendCommandToServer('read-uptime');
	socket.emit('read-time');
	window.setInterval(function() { sendCommandToServer('status'); }, STATUS_POLL_MS);
	window.setInterval(function() { socket.emit('read-time'); }, 60000);
	window.setInterval(function() { sendCommandToServer('read-uptime'); }, UPTIME_POLL_MS);
	updateTimes();
	window.setInterval(updateTimes, 1000);   // clock and "s ago" keep counting, even when paused

	document.getElementById('simulate').addEventListener('click', function() {
		sendCommandToServer('sound');
	});
	document.getElementById('stop').addEventListener('click', function() {
		if (window.confirm("Stop the master program?")) {
			sendCommandToServer('stop');
		}
	});
	document.getElementById('pause').addEventListener('click', togglePause);
	document.addEventListener('keydown', function(e) {
		// Space toggles, unless a button or field has focus (it would click that too)
		if (e.code === 'Space' && !/^(BUTTON|INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName)) {
			e.preventDefault();
			togglePause();
		}
	});
	window.addEventListener('resize', function() { drawMap(); redrawOpenSounds(); });
	if (document.fonts) {
		document.fonts.ready.then(function() { drawMap(); redrawOpenSounds(); });   // canvas text needs the web fonts first
	}
	setPauseButton();
	window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function() {
		readColors();
		mainHeat.key = "";
		drawMap();
	});
});

function readColors() {
	var css = getComputedStyle(document.documentElement);
	['map-bg', 'map-grid', 'map-grid-major', 'map-label', 'map-triangle', 'node-1', 'node-2', 'node-3',
	 'fix', 'problem', 'heat-1', 'heat-2', 'heat-low', 'heat-mid', 'heat-high'].forEach(function(name) {
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
		document.getElementById('error-box').hidden = true;
		onStatus(status);
	});

	socket.on('uptime-reply', function(message) {
		clearServerTimeout();
		var seconds = Number(message.split(" ")[0]);
		setText('board-uptime', formatDuration(seconds * 1000));
	});

	// Use the server's clock (synced), not this computer's, which may be wrong
	socket.on('time-reply', function(serverMs) {
		clockOffsetMs = Number(serverMs) - Date.now();
		updateTimes();
	});

	socket.on('sound-reply', clearServerTimeout);

	socket.on('stop-reply', function() {
		clearServerTimeout();
		setLinkState(false, "master stopped");
	});

	socket.on('master-error', errorHandler);
}

function togglePause() {
	isPaused = !isPaused;
	var button = document.getElementById('pause');
	setPauseButton();
	document.getElementById('paused-badge').hidden = !isPaused;
	if (!isPaused && pendingStatus) {
		var status = pendingStatus;
		pendingStatus = null;
		onStatus(status);
	}
}

var ICON_PAUSE = '<svg viewBox="0 0 14 14" aria-hidden="true"><rect x="2" y="1.5" width="3.5" height="11" rx="1"/><rect x="8.5" y="1.5" width="3.5" height="11" rx="1"/></svg>';
var ICON_PLAY = '<svg viewBox="0 0 14 14" aria-hidden="true"><path d="M3 1.8v10.4a.8.8 0 0 0 1.2.7l8.6-5.2a.8.8 0 0 0 0-1.4L4.2 1.1A.8.8 0 0 0 3 1.8z"/></svg>';

function setPauseButton() {
	var button = document.getElementById('pause');
	button.innerHTML = (isPaused ? ICON_PLAY + "Play" : ICON_PAUSE + "Pause");
	button.setAttribute('aria-pressed', String(isPaused));
	button.classList.toggle('paused', isPaused);
}

function onStatus(status) {
	rememberDetectionTimes(status.fixes);
	if (isPaused) {
		// Keep the board facts live; hold the map and sound panels still
		pendingStatus = status;
		setText('master-uptime', formatDuration(status.uptime_ms));
		// Node table stays as it was at pause, like Recent sounds; its
		// "Last heard" times still count up through updateTimes()
		return;
	}
	isDemo = !!status.demo;
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

	setText('fix-title', "Detected at " + formatClock(detectedAt[fix.id]));
	document.getElementById('fix-title').title = "Sound #" + fix.id;
	var age = document.getElementById('fix-age');
	age.dataset.at = detectedAt[fix.id];
	age.textContent = formatAge(nowMs() - detectedAt[fix.id]);
	var readout = document.getElementById('fix-position');
	if (fix.position) {
		readout.innerHTML = "(" + fix.position.x.toFixed(2) + ", " + fix.position.y.toFixed(2) +
			")<span class=\"unit\">m</span>";
	} else {
		readout.textContent = "no position";
	}
	if (fix.id !== lastAnnouncedId) {
		lastAnnouncedId = fix.id;
		setText('announcer', "Sound at " + formatClock(detectedAt[fix.id]) + ": " + (fix.position
			? "x " + fix.position.x.toFixed(2) + ", y " + fix.position.y.toFixed(2) + " metres, " +
			  rating.label : rating.label));
	}
	var badge = document.getElementById('fix-quality');
	badge.textContent = rating.label;
	badge.className = "quality q-" + rating.level;
	badge.hidden = false;
	setText('fix-reason', rating.reason);

	setText('fix-spread', fix.position && fix.bearings.length > 2 ? cm(fix.position.rms) : "–");
	var total = fix.bearings.reduce(function(sum, b) { return sum + b.confidence; }, 0);
	var conf = document.getElementById('fix-confidence');
	conf.textContent = fix.bearings.length > 0 ? (total / fix.bearings.length).toFixed(2) : "–";
	if (isDemo && fix.bearings.length > 0) {
		var tag = document.createElement('span');
		tag.className = 'demo-tag';
		tag.textContent = "random in demo";
		conf.appendChild(tag);
	}

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
			addCell(row, "–");
			addCell(row, "–");
			addCell(row, "0.00", 'bad');
			addCell(row, "–");
			rows.appendChild(row);
		}
	});
}

function showNodeTable(list) {
	list = list || nodes;
	var rows = document.getElementById('node-rows');
	rows.innerHTML = "";
	if (list.length === 0) {
		rows.innerHTML = '<tr><td colspan="4">No nodes yet</td></tr>';
		return;
	}
	var now = nowMs();
	list.forEach(function(node) {
		var isLive = isNodeLive(node);
		var row = document.createElement('tr');
		addNodeCell(row, node.id);
		var link = addCell(row, isLive ? "up" : "down", isLive ? 'ok' : 'bad');
		link.title = "Last message (heartbeat) " + formatAge(node.seen_ms);
		// Time since this node's last sound. Fixed when its event count changes,
		// then counted up locally (also while paused), like Recent sounds.
		var heard = addCell(row, "never");
		if (node.event_ms >= 0) {
			var seen = lastSoundAt[node.id];
			if (!seen || seen.events !== node.events) {
				seen = lastSoundAt[node.id] = {events: node.events, at: now - node.event_ms};
			}
			heard.dataset.at = seen.at;
			heard.textContent = formatAge(now - seen.at);
		}
		addCell(row, String(node.events));
		rows.appendChild(row);
	});
}

function showHistory() {
	var list = document.getElementById('history');
	var key = fixes.map(function(f) { return f.id; }).join(",") + "|" + nodes.length;
	if (key === historyKey) {
		return;              // same sounds: leave open rows alone ("ago" ticks by itself)
	}
	historyKey = key;
	list.innerHTML = "";
	if (fixes.length === 0) {
		list.innerHTML = '<li class="muted">None yet</li>';
		return;
	}
	var stillListed = {};
	fixes.forEach(function(fix) {
		stillListed[fix.id] = true;
		list.appendChild(makeHistoryItem(fix));
	});
	Object.keys(openSounds).forEach(function(id) {
		if (!stillListed[id]) {
			delete openSounds[id];      // scrolled off the master's list
		}
	});
	redrawOpenSounds();
}

// One row of Recent sounds; click it to show that sound's map, confidence
// curves and coordinates underneath
function makeHistoryItem(fix) {
	var rating = rateFix(fix, analyseLines(fix));
	var isOpen = !!openSounds[fix.id];
	var item = document.createElement('li');
	item.dataset.fixId = fix.id;

	var row = document.createElement('button');
	row.type = 'button';
	row.className = 'sound-row';
	row.title = "Sound #" + fix.id + ": show its map";
	row.setAttribute('aria-expanded', String(isOpen));
	row.setAttribute('aria-controls', 'sound-' + fix.id);

	var what = document.createElement('span');
	var dot = document.createElement('span');
	dot.className = "qdot q-" + rating.level;
	dot.title = rating.label;
	what.appendChild(dot);
	var label = document.createElement('span');
	label.className = 'sr-only';
	label.textContent = rating.label + ": ";
	what.appendChild(label);
	what.appendChild(document.createTextNode(formatClock(detectedAt[fix.id])));
	var where = document.createElement('span');
	where.textContent = fix.position
		? "(" + fix.position.x.toFixed(2) + ", " + fix.position.y.toFixed(2) + ")"
		: "no fix";
	var count = document.createElement('span');
	count.className = 'lines-count';
	count.textContent = fix.bearings.length + " line" + (fix.bearings.length === 1 ? "" : "s");
	var when = document.createElement('span');
	when.className = 'when';
	when.dataset.at = detectedAt[fix.id];
	when.textContent = formatAge(nowMs() - detectedAt[fix.id]);
	var chevron = document.createElement('span');
	chevron.className = 'chevron';
	chevron.setAttribute('aria-hidden', 'true');
	[what, where, count, when, chevron].forEach(function(el) { row.appendChild(el); });

	var detail = document.createElement('div');
	detail.className = 'sound-detail';
	detail.id = 'sound-' + fix.id;
	detail.hidden = !isOpen;
	var recorded = document.createElement('p');
	recorded.className = 'detail-time';
	recorded.textContent = "Recorded at " + formatClock(detectedAt[fix.id]);
	var coords = document.createElement('p');
	coords.className = 'detail-coords';
	if (fix.position) {
		coords.innerHTML = "(" + fix.position.x.toFixed(2) + ", " + fix.position.y.toFixed(2) +
			")<span class=\"unit\">m</span>";
	} else {
		coords.textContent = "No position: fewer than two lines crossed.";
	}
	var map = document.createElement('canvas');
	map.className = 'detail-map';
	map.setAttribute('aria-label', "Map of this sound");
	var chart = document.createElement('canvas');
	chart.className = 'chart detail-chart';
	chart.setAttribute('aria-label', "Confidence per node for this sound");
	[recorded, coords, map, chart].forEach(function(el) { detail.appendChild(el); });

	row.addEventListener('click', function() {
		var open = !openSounds[fix.id];
		if (open) {
			openSounds[fix.id] = {heat: {canvas: document.createElement('canvas'), key: ""}};
		} else {
			delete openSounds[fix.id];
		}
		row.setAttribute('aria-expanded', String(open));
		detail.hidden = !open;
		if (open) {
			drawSoundDetail(item, fix);
		}
	});

	item.appendChild(row);
	item.appendChild(detail);
	return item;
}

function drawSoundDetail(item, fix) {
	var state = openSounds[fix.id];
	if (!state) {
		return;
	}
	var mainView = view;          // renderMap sets the shared view; put it back after
	renderMap(item.querySelector('.detail-map'), fix, state.heat, false);
	drawConfidence(item.querySelector('.detail-chart'), fix);
	view = mainView;
}

function redrawOpenSounds() {
	document.querySelectorAll('#history li[data-fix-id]').forEach(function(item) {
		var id = item.dataset.fixId;
		if (openSounds[id]) {
			var fix = fixes.find(function(f) { return String(f.id) === id; });
			if (fix) {
				drawSoundDetail(item, fix);
			}
		}
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
	var fix = fixes.length > 0 ? fixes[0] : null;
	renderMap(document.getElementById('map'), fix, mainHeat, true);
	drawConfidence(document.getElementById('confidence'), fix);
}

// The map for one sound. withTrail adds the earlier sounds as fading dots.
function renderMap(canvas, fix, heatCache, withTrail) {
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
	if (withTrail) {
		drawTrail(ctx);
	}

	if (fix) {
		drawHeat(ctx, fix, heatCache);
		fix.bearings.forEach(function(b) {
			var node = nodesById[b.node];
			if (node && b.confidence > 0) {     // no line at confidence 0
				drawRay(ctx, node, b.bearing, rayLength(node, b.bearing, fix), nodeColor(node.id));
			}
		});
		if (fix.position) {
			drawEstimate(ctx, fix);
		}
	}
	nodes.forEach(function(node) { drawNode(ctx, node); });
}

/* ---------- confidence curves under the map ---------- */

// One bell curve per node for the sound on the map: centred on the angle it
// heard (theta), as wide as the heat map treats that line (narrower = more sure).
// Area 1 each, so a confident node is tall and thin, an unsure one low and wide.
function drawConfidence(canvas, fix) {
	var ctx = canvas.getContext('2d');
	var ratio = window.devicePixelRatio || 1;
	var w = canvas.clientWidth, h = canvas.clientHeight;
	if (w === 0 || h === 0) {
		return;
	}
	if (canvas.width !== Math.round(w * ratio) || canvas.height !== Math.round(h * ratio)) {
		canvas.width = Math.round(w * ratio);
		canvas.height = Math.round(h * ratio);
	}
	ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
	ctx.fillStyle = colors['map-bg'];
	ctx.fillRect(0, 0, w, h);

	// Nodes with no curve get a note in a row of their own above the plot
	var byNode = {};
	if (fix) {
		fix.bearings.forEach(function(b) { byNode[b.node] = b; });
	}
	var missing = nodes.filter(function(n) { return !byNode[n.id] || !(byNode[n.id].confidence > 0); });
	var pad = {left: 26, right: 26, top: 24 + (missing.length > 0 ? 18 : 0), bottom: 26};
	var plotW = w - pad.left - pad.right, plotH = h - pad.top - pad.bottom;
	var toX = function(deg) { return pad.left + (deg + 90) / 180 * plotW; };
	var peakMax = 1 / (HEAT_SIGMA_DEG * Math.sqrt(2 * Math.PI));   // tallest possible (confidence 1)
	var toY = function(pdf) { return pad.top + plotH * (1 - pdf / peakMax); };

	// Axis: theta every 30 degrees
	ctx.font = "12px " + CANVAS_FONT;
	ctx.textAlign = 'center';
	ctx.lineWidth = 1;
	for (var t = -90; t <= 90; t += 30) {
		var px = Math.round(toX(t)) + 0.5;
		ctx.strokeStyle = colors[t === 0 ? 'map-grid-major' : 'map-grid'];
		line(ctx, px, pad.top, px, pad.top + plotH);
		ctx.fillStyle = colors['map-label'];
		ctx.fillText((t > 0 ? "+" : "") + t + "°", px, h - 8);
	}
	ctx.strokeStyle = colors['map-grid-major'];
	line(ctx, pad.left, pad.top + plotH + 0.5, pad.left + plotW, pad.top + plotH + 0.5);

	var labels = [];
	nodes.forEach(function(node) {
		var b = byNode[node.id];
		var color = nodeColor(node.id);
		if (!b || !(b.confidence > 0)) {
			return;
		}
		var sigma = HEAT_SIGMA_DEG / Math.max(b.confidence, HEAT_MIN_CONFIDENCE);
		var pdf = function(x) {
			var z = (x - b.theta) / sigma;
			return Math.exp(-0.5 * z * z) / (sigma * Math.sqrt(2 * Math.PI));
		};
		// Curve only (stroke), so the baseline stays neutral; fill underneath it
		var curve = new Path2D();
		for (var x = -90; x <= 90; x += 0.5) {
			if (x === -90) {
				curve.moveTo(toX(x), toY(pdf(x)));
			} else {
				curve.lineTo(toX(x), toY(pdf(x)));
			}
		}
		var area = new Path2D(curve);
		area.lineTo(toX(90), toY(0));
		area.lineTo(toX(-90), toY(0));
		area.closePath();
		ctx.globalAlpha = 0.16;
		ctx.fillStyle = color;
		ctx.fill(area);
		ctx.globalAlpha = 1;
		ctx.strokeStyle = color;
		ctx.lineWidth = 2;
		ctx.stroke(curve);

		// Peak marker and label
		var peakX = toX(b.theta), peakY = toY(pdf(b.theta));
		ctx.setLineDash([3, 3]);
		ctx.lineWidth = 1;
		line(ctx, peakX, peakY, peakX, pad.top + plotH);
		ctx.setLineDash([]);
		labels.push({text: "N" + node.id + " " + b.confidence.toFixed(2), x: peakX, y: Math.max(peakY - 7, 14), color: color});
	});

	// Peak labels, nudged up when two would overlap
	ctx.font = "600 12px " + CANVAS_SANS;
	ctx.textAlign = 'center';
	labels.sort(function(a, b) { return a.x - b.x; });
	labels.forEach(function(l, i) {
		l.w = ctx.measureText(l.text).width + 8;
		for (var j = 0; j < i; j++) {
			var o = labels[j];
			if (Math.abs(o.x - l.x) < (o.w + l.w) / 2 && Math.abs(o.y - l.y) < 14) {
				l.y = (o.y - 15 >= 12) ? o.y - 15 : o.y + 15;   // above if there is room, else below
			}
		}
		l.x = Math.min(Math.max(l.x, l.w / 2), w - l.w / 2);
		ctx.fillStyle = l.color;
		ctx.fillText(l.text, l.x, Math.max(l.y, missing.length > 0 ? 30 : 12));
	});
	ctx.font = "12px " + CANVAS_FONT;

	// Nodes with no curve, top left
	ctx.textAlign = 'left';
	var noteX = pad.left;
	missing.forEach(function(n) {
		var text = "N" + n.id + ": confidence 0";
		ctx.fillStyle = nodeColor(n.id);
		ctx.fillText(text, noteX, 15);
		noteX += ctx.measureText(text).width + 16;
	});
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
	ctx.fillStyle = colors['map-label'];
	ctx.font = "12px " + CANVAS_FONT;
	var labelAll = GRID_STEP_M * view.scale >= GRID_LABEL_MIN_PX;
	// Label every major line, or every other one when they would crowd (small screens)
	var labelEvery = GRID_MAJOR_EVERY * (GRID_STEP_M * GRID_MAJOR_EVERY * view.scale >= 44 ? 1 : 2);
	// Whole-number line indices so 0.1 steps don't pile up rounding error
	var first = Math.ceil(view.left / GRID_STEP_M - 1e-9);
	var last = Math.floor((view.left + view.span) / GRID_STEP_M + 1e-9);
	for (var i = first; i <= last; i++) {
		var x = i * GRID_STEP_M;
		var isMajor = i % GRID_MAJOR_EVERY === 0;
		var px = Math.round(view.toCanvas(x, 0).x) + 0.5;
		ctx.strokeStyle = colors[isMajor ? 'map-grid-major' : 'map-grid'];
		line(ctx, px, 0, px, size);
		if (labelAll || i % labelEvery === 0) {
			ctx.fillText(x.toFixed(1), px + 3, size - 5);
		}
	}
	first = Math.ceil(view.bottom / GRID_STEP_M - 1e-9);
	last = Math.floor((view.bottom + view.span) / GRID_STEP_M + 1e-9);
	for (var j = first; j <= last; j++) {
		var y = j * GRID_STEP_M;
		var isMajorY = j % GRID_MAJOR_EVERY === 0;
		var py = Math.round(view.toCanvas(0, y).y) + 0.5;
		ctx.strokeStyle = colors[isMajorY ? 'map-grid-major' : 'map-grid'];
		line(ctx, 0, py, size, py);
		if ((labelAll || j % labelEvery === 0) && py < size - 24) {   // keep clear of the x labels
			ctx.fillText(y.toFixed(1) + " m", 4, py - 4);
		}
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
		ctx.fillStyle = colors.problem;
		ctx.font = "600 15px " + CANVAS_SANS;
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
	ctx.font = "600 12px " + CANVAS_SANS;
	ctx.textAlign = "center";
	ctx.textBaseline = "middle";
	ctx.fillText("N" + node.id, p.x, p.y + 0.5);
	ctx.textAlign = "start";
	ctx.textBaseline = "alphabetic";
}

/* ---------- heat map ---------- */

function drawHeat(ctx, fix, cache) {
	var key = fix.id + "|" + view.size + "|" + view.left + "|" + view.bottom + "|" + view.span;
	if (key !== cache.key) {
		cache.key = key;
		buildHeat(fix, cache.canvas);
	}
	ctx.save();
	ctx.globalCompositeOperation = 'multiply';   // light map: the colours darken the paper
	ctx.imageSmoothingEnabled = true;
	ctx.drawImage(cache.canvas, 0, 0, view.size, view.size);
	ctx.restore();
}

// For every cell: how well do all the lines agree that the sound came from here?
// Each line counts as a Gaussian in angle, HEAT_SIGMA_DEG wide at confidence 1
// and wider when the node is less sure. Multiplying them leaves the region where
// every line points at once. Brightness is relative to the best cell.
function buildHeat(fix, heatCanvas) {
	heatCanvas.width = HEAT_CELLS;
	heatCanvas.height = HEAT_CELLS;
	var hctx = heatCanvas.getContext('2d');
	var image = hctx.createImageData(HEAT_CELLS, HEAT_CELLS);

	var lines = [];
	fix.bearings.forEach(function(b) {
		var node = nodesById[b.node];
		if (node && b.confidence > 0) {
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

	// Yellow (weak) -> orange -> red (where the lines agree best)
	var ramp = [hexToRgb(colors['heat-low']), hexToRgb(colors['heat-mid']), hexToRgb(colors['heat-high'])];
	for (var j = 0; j < scores.length; j++) {
		var v = Math.exp(scores[j] - best);     // 1 where the lines agree best
		var c = rampColor(ramp, v);
		var px = j * 4;
		image.data[px]     = c.r;
		image.data[px + 1] = c.g;
		image.data[px + 2] = c.b;
		image.data[px + 3] = 230 * Math.pow(v, 0.45);
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

// Colour at t (0 to 1) along evenly spaced stops
function rampColor(stops, t) {
	var x = Math.min(Math.max(t, 0), 1) * (stops.length - 1);
	var i = Math.min(Math.floor(x), stops.length - 2);
	var f = x - i;
	var a = stops[i], b = stops[i + 1];
	return {r: a.r + (b.r - a.r) * f, g: a.g + (b.g - a.g) * f, b: a.b + (b.b - a.b) * f};
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

// The master only says how long ago each sound was; turn that into a time once,
// on first sight, so it doesn't jitter between polls
function rememberDetectionTimes(list) {
	var now = nowMs();
	var keep = {};
	list.forEach(function(fix) {
		keep[fix.id] = detectedAt[fix.id] !== undefined ? detectedAt[fix.id] : now - fix.age_ms;
	});
	fixes.forEach(function(fix) {      // the ones on screen, in case we are paused
		if (keep[fix.id] === undefined && detectedAt[fix.id] !== undefined) {
			keep[fix.id] = detectedAt[fix.id];
		}
	});
	detectedAt = keep;
}

// Clock, plus every "s ago" on the page
function updateTimes() {
	var now = nowMs();
	setText('clock', formatClock(now));
	document.querySelectorAll('[data-at]').forEach(function(el) {
		el.textContent = formatAge(now - Number(el.dataset.at));
	});
}

function nowMs() {
	return Date.now() + clockOffsetMs;
}

// HH:MM:SS at DISPLAY_UTC_OFFSET_H. Done by hand so the viewer's own time zone
// settings can't change it.
function formatClock(ms) {
	var d = new Date(ms + DISPLAY_UTC_OFFSET_H * 3600000);
	return [d.getUTCHours(), d.getUTCMinutes(), d.getUTCSeconds()].map(function(n) {
		return (n < 10 ? "0" : "") + n;
	}).join(":");
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
	setText('error-text', String(message).replace(/^(SERVER )?ERROR:\s*/, ""));
	setLinkState(false);
	document.getElementById('error-box').hidden = false;
	clearServerTimeout();
}
