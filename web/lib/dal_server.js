"use strict";
// Relays websocket commands from the browser to the master program over UDP
// (ported from Assignment 3's beatbox_server.js).
//
//   browser event   UDP command   reply event
//   'status'        "status"      'status-reply' (JSON text)
//   'sound'         "sound"       'sound-reply' (demo mode: fake a sound now)
//   'stop'          "stop"        'stop-reply'
//   'read-uptime'   (reads /proc/uptime)  'uptime-reply'
//   'read-time'     (this machine's clock) 'time-reply' (ms since 1970, UTC)
// Any failure is sent to the browser as 'master-error'.

var fs       = require('fs');
var dgram    = require('dgram');
var socketio = require('socket.io');

var MASTER_PORT = 12345;       // must match UDP_PORT in master/include/udp.h
var MASTER_HOST = '127.0.0.1';
var REPLY_TIMEOUT_MS = 1000;

exports.listen = function(server) {
	var io = socketio(server);
	io.on('connection', function(socket) {
		handleCommands(socket);
	});
};

function handleCommands(socket) {
	console.log("Browser connected.");

	socket.on('read-uptime', function() {
		readAndSendFile(socket, '/proc/uptime', 'uptime-reply');
	});
	socket.on('read-time', function() {
		socket.emit('time-reply', Date.now());
	});
	socket.on('status', function() {
		relayToMaster(socket, "status", "status-reply");
	});
	socket.on('sound', function() {
		relayToMaster(socket, "sound", "sound-reply");
	});
	socket.on('stop', function() {
		console.log("Got stop command");
		relayToMaster(socket, "stop", "stop-reply");
	});
}

function readAndSendFile(socket, absPath, replyName) {
	fs.readFile(absPath, function(err, fileData) {
		if (err) {
			socket.emit('master-error', "ERROR: Unable to read file " + absPath);
		} else if (fileData.length > 0) {
			socket.emit(replyName, fileData.toString('utf8'));
		}
	});
}

function relayToMaster(socket, command, replyName) {
	var client = dgram.createSocket('udp4');
	var buffer = Buffer.from(command);

	// Report an error if the master doesn't answer in time
	var errorTimer = setTimeout(function() {
		socket.emit('master-error', "SERVER ERROR: No response from the master program. Is it running?");
		client.close();
	}, REPLY_TIMEOUT_MS);

	client.on('message', function(message) {
		clearTimeout(errorTimer);
		socket.emit(replyName, message.toString('utf8'));
		client.close();
	});
	client.on('error', function(err) {
		clearTimeout(errorTimer);
		socket.emit('master-error', "SERVER ERROR: UDP " + err.message);
		client.close();
	});

	client.send(buffer, 0, buffer.length, MASTER_PORT, MASTER_HOST, function(err) {
		if (err) {
			clearTimeout(errorTimer);
			socket.emit('master-error', "SERVER ERROR: UDP send failed: " + err.message);
			client.close();
		}
	});
}
