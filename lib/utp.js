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
        this._handle = null;
        server.emit('close');
    });
    handle._onError = BlockError(function (err) {
        server.emit('error', err);
    });
}

function Server() {
    var self = this;
    if (!(self instanceof Server)) self = Object.create(utp.Server.prototype);
    var argIndex = 0, connectionListener;
    if (typeof arguments[argIndex] === 'object') var options = arguments[argIndex++];
    if (typeof arguments[argIndex] === 'function') var connectionListener = arguments[argIndex++];
    EventEmitter.call(self);
    if (connectionListener) self.on('connection', connectionListener);
    return self;
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
    return this;
};

Server.prototype.close = function (callback) {
    if (typeof callback !== 'function') callback = () => {};
    if (this._handle.state() !== 'STATE_STOPPED') {
        this.once('close', callback);
        this._handle.close();
    } else {
        process.nextTick(callback);
    }
    return this;
};

Server.prototype.ref = function () {
    this._handle.ref();
    return this;
};

Server.prototype.address = function () {
    if (!this._handle) return null;
    return this._handle.address();
};

Server.prototype.unref = function () {
    this._handle.unref();
    return this;
};

function Socket() {
    var self = this;
    if (!(self instanceof Socket)) self = Object.create(Socket.prototype);
    stream.Duplex.call(self);
    return self;
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
        if (this._monoContext) this._context.close();
        this._handle = null;
        this._context = null;
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
    _context: null,
    _monoContext: false
};
util.inherits(Socket, stream.Duplex);

Socket.prototype._write = function (chunk, encoding, callback) {
    this._cachedChunk = chunk;
    this._cachedCallback = callback;
    return this;
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
                    this._monoContext = true;
                }
                this._context = context;
                UTPSocketFactory(this, context.connect(port, address));
                this.emit('listening');
            } catch (err) {
                this.emit('error', err);
            }
        }
    });
    return this;
};

Socket.prototype.ref = function () {
    this._handle.ref();
    return this;
};

Socket.prototype.unref = function () {
    this._handle.unref();
    return this;
};

Socket.prototype.address = function () {
    if (!this._context) return null;
    else return this._context.address();
};

Object.defineProperty(Socket.prototype, 'remoteAddress', {
    enumerable: true,
    get: function () {
        if (!this._handle) return null;
        var remoteAddr = this._handle.remoteAddress();
        if (!remoteAddr) return null;
        return remoteAddr.address;
    }
});

Object.defineProperty(Socket.prototype, 'remoteFamily', {
    enumerable: true,
    get: function () {
        if (!this._handle) return null;
        var remoteAddr = this._handle.remoteAddress();
        if (!remoteAddr) return null;
        return remoteAddr.family;
    }
});

Object.defineProperty(Socket.prototype, 'remotePort', {
    enumerable: true,
    get: function () {
        if (!this._handle) return null;
        var remoteAddr = this._handle.remoteAddress();
        if (!remoteAddr) return null;
        return remoteAddr.port;
    }
});

utp.createServer = utp.Server.bind(null);
utp.connect = utp.createConnection = function () {
    var socket = new Socket();
    socket.connect.apply(socket, arguments);
    return socket;
};


process.on('exit', () => {
    libutp.cleanup();
})

// SIGINT Hack
function onSigInt() {
    process.exit();
};
function onNewEvent(event, listener) {
    if (event === 'SIGINT') {
        removeSIGINTHack();
    }
}
if (process.listenerCount('SIGINT') === 0) {
    process.on('SIGINT', onSigInt);
    process.on('newListener', onNewEvent);
}
function removeSIGINTHack() {
    process.removeListener('SIGINT', onSigInt);
    process.removeListener('newListener', onNewEvent);
}
utp.removeSIGINTHack = removeSIGINTHack;
