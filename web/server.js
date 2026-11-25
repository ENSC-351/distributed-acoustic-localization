"use strict";
// HTTP server for the master's web GUI (ported from Assignment 3).
// Serves public/ and hands websocket traffic to lib/dal_server.js,
// which relays it to the master over UDP.
//
// Install modules once:  npm install
// Launch with:           node server.js
// Then browse to         http://<board IP>:8088

var PORT_NUMBER = 8088;

var http = require('http');
var fs   = require('fs');
var path = require('path');

var PUBLIC_DIR = path.join(__dirname, 'public');

var MIME_TYPES = {
	'.html': 'text/html',
	'.css':  'text/css',
	'.js':   'application/javascript',
	'.svg':  'image/svg+xml',
	'.png':  'image/png',
	'.ico':  'image/x-icon'
};

var server = http.createServer(function(request, response) {
	var urlPath;
	try {
		urlPath = decodeURIComponent(request.url.split('?')[0]);
	} catch (e) {
		send404(response);   // malformed %-escape; don't let it crash the server
		return;
	}
	if (urlPath === '/') {
		urlPath = '/index.html';
	}
	// Resolve inside public/ only, so "/../server.js" can't escape it
	var absPath = path.join(PUBLIC_DIR, path.normalize(urlPath));
	if (absPath.indexOf(PUBLIC_DIR + path.sep) !== 0) {
		send404(response);
		return;
	}
	serveStatic(response, absPath);
});

server.listen(PORT_NUMBER, function() {
	console.log("Server listening on port " + PORT_NUMBER);
});

function serveStatic(response, absPath) {
	fs.readFile(absPath, function(err, data) {
		if (err) {
			send404(response);
		} else {
			var type = MIME_TYPES[path.extname(absPath)] || 'application/octet-stream';
			response.writeHead(200, {'Content-Type': type});
			response.end(data);
		}
	});
}

function send404(response) {
	response.writeHead(404, {'Content-Type': 'text/plain'});
	response.end('Error 404: resource not found.');
}

// Websocket relay to the master's UDP server
require('./lib/dal_server').listen(server);
