var net = require('net');
var util = require('util');
var EventEmitter = require('events').EventEmitter;
var stream = require('stream');
var dns = require('dns');
var assert = require('assert');
var libutp = require('bindings')('node-libutp.node');

var utp = module.exports;
utp.Server = Server;
utp.Socket = Socket;

function BlockError (func) {
    return function () {
        try {
            return func.apply(this, arguments);
        } catch (err) {
            console.error('Unhandled Exception:');
            console.error(err.stack);
        }
    }
}

function UTPContextFactory(server, handle) {
    server._handle = handle;
    handle._onConnection = BlockError(function (_handle) {
        var socket = new Socket();
        var connTimer = setTimeout(() => _handle.forceTimedOut(), 29000);
        UTPSocketFactory(socket, _handle, connTimer);
        server.emit('connection', socket);
    });
    handle._onClose = BlockError(function () {
        server.emit('close');
    });
    handle._onError = BlockError(function (err) {
        server.emit('error', err);
    });
}

function Server() {
    var argIndex = 0, connectionListener;
    if (typeof arguments[argIndex] === 'object') var options = arguments[argIndex++];
    if (typeof arguments[argIndex] === 'function') var connectionListener = arguments[argIndex++];
    EventEmitter.call(this);
    if (connectionListener) this.on('connection', connectionListener);
};
util.inherits(Server, EventEmitter);

Server.prototype.listen = function () {
    var port = 0, host = '::', backlog = 511, callback;
    if (typeof arguments[0] === 'object') {
        var options = arguments[0];
        if (typeof options.port === 'number') port = options.port;
        if (typeof options.host === 'string') host = options.host;
        if (typeof options.backlog === 'number') backlog = options.backlog;
        callback = arguments[1];
    } else {
        var argIndex = 0;
        if (typeof arguments[argIndex] === 'number') port = arguments[argIndex++];
        if (typeof arguments[argIndex] === 'string') host = arguments[argIndex++];
        if (typeof arguments[argIndex] === 'number') backlog = arguments[argIndex++];
        if (typeof arguments[argIndex] === 'function') callback = arguments[argIndex++];
    }
    port = port | 0;
    backlog = backlog | 0;
    if (port === 0) port = parseInt(Math.random() * (65536 - 1024) + 1024);
    assert(port < 65536 && port > 0);
    if (callback) this.once('listening', callback);
    dns.lookup(host, (err, address, family) => {
        if (err) {
            this.emit('error', err);
        } else {
            try {
                if (!this._handle) UTPContextFactory(this, new libutp.UTPContext());
                if (this._handle.state() === 'STATE_INIT') this._handle.bind(port, address);
                if (this._handle.state() === 'STATE_BOUND') this._handle.listen(backlog);
                this.emit('listening');
            } catch (err) {
                this.emit('error', err);
            }
        }
    });
};

Server.prototype.close = function (callback) {
    if (this._handle.state() !== 'STATE_STOPPED') {
        this.once('close', callback);
        this._handle.close();
    } else {
        process.nextTick(callback);
    }
};

function Socket() {
    stream.Duplex.call(this);
};

function UTPSocketFactory(socket, handle, timer) {
    socket._handle = handle;
    var had_error = false;
    handle._onConnect = BlockError(function () {
        if (timer) clearTimeout(timer);
        if (socket._cachedChunk) {
            handle.write(socket._cachedChunk, () => {
                socket._cachedCallback();
                delete socket._cachedChunk;
                delete socket._cachedCallback;
            });
        }
        socket.emit('connect');
    });
    handle._onEnd = BlockError(function () {
        socket.push(null);
    });
    handle._onError = BlockError(function (err) {
        had_error = true;
        socket.emit('error', err);
    });
    handle._onDestroy = BlockError(function () {
        socket.emit('close', had_error);
    });
    handle._onRead = BlockError(function (buf) {
        socket.push(buf);
    });
    socket._read = function (size) {
    };
    socket._write = function (chunk, encoding, callback) {
        handle.write(chunk, callback);
    };
    socket.end = function (chunk, encoding) {
        stream.Duplex.prototype.end.call(socket, chunk, encoding, () => handle.close());
    };
}

Socket.prototype = {
    _readLimit: 0,
    _handle: null,
};
util.inherits(Socket, stream.Duplex);

Socket.prototype._write = function (chunk, encoding, callback) {
    this._cachedChunk = chunk;
    this._cachedCallback = callback;
}

Socket.prototype.connect = function () {
    var port, host = '::1', localPort = 0, localAddress = '::', connectListener, context;
    if (typeof arguments[0] === 'object') {
        var options = arguments[0];
        if (typeof options.port === 'number') port = options.port;
        if (typeof options.host === 'string') host = options.host;
        if (typeof options.localPort === 'number') localPort = options.localPort;
        if (typeof options.localAddress === 'string') localAddress = options.localAddress;
        if (options.server instanceof Server) context = options.server._handle;
        connectListener = arguments[1];
    } else {
        var argIndex = 0;
        if (typeof arguments[argIndex] === 'number') port = arguments[argIndex++];
        if (typeof arguments[argIndex] === 'string') host = arguments[argIndex++];
        if (typeof arguments[argIndex] === 'function') connectListener = arguments[argIndex++];
    }
    localPort = localPort | 0;
    if (localPort === 0) localPort = parseInt(Math.random() * (65536 - 16384) + 16384);
    assert(typeof port === 'number' && port < 65536 && port > 0);
    assert(localPort < 65536 && localPort > 0);
    if (connectListener) this.on('connect', connectListener);
    dns.lookup(host, (err, address, family) => {
        if (err) {
            this.emit('error', err);
        } else {
            try {
                if (!context) {
                    context = new libutp.UTPContext();
                    context._onError = (err) => this.emit('error', err);
                    context.bind(localPort, family === 4 ? '0.0.0.0' : '::');
                }
                UTPSocketFactory(this, context.connect(port, address));
                context.close();
                this.emit('listening');
            } catch (err) {
                this.emit('error', err);
            }
        }
    });
};
