#include "utp.h"

namespace nodeUTP {

const char *UTPContext::statestr[] = {"STATE_INIT", "STATE_BOUND", "STATE_STOPPED"};
Nan::Persistent<v8::Function> UTPContext::constructor;

UTPContext::UTPContext():
ctx(utp_init(2), [] (utp_context *ctx) { if (ctx) utp_destroy(ctx); }),
state(STATE_INIT),
listening(false),
backlog(0),
connections(0),
pendingConnections(0),
refCount(0),
refSelf(false)
{
	assert(uv_udp_init(uv_default_loop(), &udpHandle) >= 0);
	assert(uv_timer_init(uv_default_loop(), &timerHandle) >= 0);

	utp_context_set_userdata(ctx.get(), this);
	udpHandle.data = this;
	timerHandle.data = this;

	for (int type: vector<int>({UTP_SENDTO, UTP_ON_ERROR, UTP_ON_STATE_CHANGE, UTP_ON_READ, UTP_ON_FIREWALL, UTP_ON_ACCEPT})) {
		utp_set_callback(ctx.get(), type, [] (utp_callback_arguments *a) {
			UTPContext *utpctx = static_cast<UTPContext *>(utp_context_get_userdata(a->context));
			return utpctx->onCallback(a);
		});
	}

#ifdef _DEBUG
	utp_context_set_option(ctx.get(), UTP_LOG_NORMAL, 1);
	utp_context_set_option(ctx.get(), UTP_LOG_MTU,    1);
	utp_context_set_option(ctx.get(), UTP_LOG_DEBUG,  1);
	utp_set_callback(ctx.get(), UTP_LOG, [] (utp_callback_arguments *a) -> uint64 {
		//std::cout << a->buf << std::endl;
		return 0;
	});
#endif

}

UTPContext::~UTPContext() {
}

void UTPContext::uvRef() {
	refSelf = true;
	uv_ref(reinterpret_cast<uv_handle_t *>(&udpHandle));
	uv_ref(reinterpret_cast<uv_handle_t *>(&timerHandle));
}

void UTPContext::uvUnref() {
	refSelf = false;
	if (refCount == 0)	{
		uv_unref(reinterpret_cast<uv_handle_t *>(&udpHandle));
		uv_unref(reinterpret_cast<uv_handle_t *>(&timerHandle));
	}
}

void UTPContext::sockRef() {
	refCount++;
	uv_ref(reinterpret_cast<uv_handle_t *>(&udpHandle));
	uv_ref(reinterpret_cast<uv_handle_t *>(&timerHandle));
}

void UTPContext::sockUnref() {
	assert(refCount);
	refCount--;
	if (!refSelf)	{
		uv_unref(reinterpret_cast<uv_handle_t *>(&udpHandle));
		uv_unref(reinterpret_cast<uv_handle_t *>(&timerHandle));
	}
}

/* return libuv errro code */
int UTPContext::bind(uint16_t port, string host) {
	assert(state == STATE_INIT);
	union {
		struct sockaddr saddr;
		struct sockaddr_in sin;
		struct sockaddr_in6 sin6;
	} addr;
	int family = AF_INET;
	int errcode = uv_ip4_addr(host.c_str(), port, &addr.sin);
	if (errcode < 0) {
		errcode = uv_ip6_addr(host.c_str(), port, &addr.sin6);
		family = AF_INET6;
	}
	if (errcode < 0) return errcode;
	errcode = uv_udp_bind(&udpHandle, &addr.saddr, UV_UDP_REUSEADDR);
	if (errcode < 0) return errcode;

#ifdef __linux__
	uv_os_sock_t newsock = socket(family, SOCK_DGRAM, IPPROTO_UDP);
	int on = 1;
	assert(setsockopt(newsock, SOL_IP, IP_RECVERR, &on, sizeof(on)) == 0);
	assert(uv_udp_open(&udpHandle, newsock >= 0));
#endif

	assert(uv_udp_recv_start(&udpHandle, static_cast<void (*)(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)> (
		[] (uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
		    buf->base = new (nothrow) char[suggested_size];
			assert(buf->base);
		    buf->len = suggested_size;
		}
	), [] (uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *addr, unsigned flags) {
		UTPContext *utpctx = static_cast<UTPContext *>(handle->data);
		if (!utpctx->ctx.get()) return;
		utpctx->uvRecv(nread, buf->base, addr, flags);
		delete[] buf->base;
		utp_check_timeouts(utpctx->ctx.get());
	}) == 0);
	assert(uv_timer_start(&timerHandle, static_cast<void (*)(uv_timer_t *handle)> ([] (uv_timer_t *handle) -> void {
		UTPContext *utpctx = static_cast<UTPContext *>(handle->data);
		if (!utpctx->ctx.get()) return;
		utp_check_timeouts(utpctx->ctx.get());
	}), 0, 500) >= 0);
	uvUnref();
	Ref();
	state = STATE_BOUND;
	return 0;
};

int UTPContext::connect(uint16_t port, string host, UTPSocket **putpsock) {
	assert(state == STATE_BOUND);
	utp_socket *sock = nullptr;
	union {
		struct sockaddr saddr;
		struct sockaddr_in sin;
		struct sockaddr_in6 sin6;
	} addr;
	int errcode = 0;
	if (uv_ip4_addr(host.c_str(), port, &addr.sin) >= 0) {
		sock = utp_create_socket(ctx.get());
		utp_connect(sock, &addr.saddr, sizeof(struct sockaddr_in));
	} else if ((errcode = uv_ip6_addr(host.c_str(), port, &addr.sin6)) >= 0) {
		sock = utp_create_socket(ctx.get());
		utp_connect(sock, &addr.saddr, sizeof(struct sockaddr_in6));
	} else {
		return errcode;
	}
	connections++;
	pendingConnections++;
	*putpsock = new UTPSocket(this, sock);
	return 0;
}

void UTPContext::listen(int _backlog) {
	if (state != STATE_BOUND) return;
	backlog = _backlog;
	if (backlog < 0) backlog = 0;
	uvRef();
	listening = true;
	return;
}

void UTPContext::destroy() {
	listening = false;
	//std::cout << "try destroy" << std::endl;
	uvUnref();
	state = STATE_STOPPED;
	if (connections == 0 && state == STATE_STOPPED) {
		//std::cout << "destroy" << std::endl;
		Nan::HandleScope scope;
		v8::Local<v8::Function> onClose = Nan::Get(handle(), Nan::New("_onClose").ToLocalChecked()).ToLocalChecked().As<v8::Function>();
		Nan::Callback(onClose).Call(0, 0);
		uv_unref(reinterpret_cast<uv_handle_t *>(&udpHandle));
		uv_unref(reinterpret_cast<uv_handle_t *>(&timerHandle));
		Unref();
		MakeWeak();
		assert(uv_timer_stop(&timerHandle) >= 0);
		assert(uv_udp_recv_stop(&udpHandle) >= 0);
		uv_close(reinterpret_cast<uv_handle_t *>(&udpHandle), nullptr);
		uv_close(reinterpret_cast<uv_handle_t *>(&timerHandle), nullptr);
		//ctx.reset(nullptr); // bug: will check timeout after releasing the object (why?)
	}
}

uint64 UTPContext::onCallback(utp_callback_arguments *a) {
	UTPSocket *utpsock;
	switch (a->callback_type) {
	case UTP_SENDTO:
		return sendTo(a->buf, a->len, a->address, a->address_len);
	case UTP_ON_FIREWALL:
		return static_cast<uint64>(onFirewall());
	case UTP_ON_ACCEPT:
		connections++;
		pendingConnections++;
		onAccept(a->socket);
		return 0;

	case UTP_ON_READ:
		utpsock = UTPSocket::get(a->socket);
		UTPSocket::get(a->socket)->onRead(a->buf, a->len);
		return 0;
	case UTP_ON_STATE_CHANGE:
		utpsock = UTPSocket::get(a->socket);
		switch (a->state) {
		case UTP_STATE_CONNECT:
			pendingConnections--;
			utpsock->onConnect();
			return 0;
		case UTP_STATE_WRITABLE:
			utpsock->onWritable();
			return 0;
		case UTP_STATE_EOF:
			utpsock->onEnd();
			return 0;
		case UTP_STATE_DESTROYING:
			utpsock->onDestroy();
			connections--;
			if (state == STATE_STOPPED) destroy();
			return 0;
		}
		return 0;
	case UTP_ON_ERROR:
		utpsock = UTPSocket::get(a->socket);
		utpsock->onError(a->error_code);
		return 0;
	}
	return 0;
}

void UTPContext::uvRecv(ssize_t len, const void *buf, const struct sockaddr *addr, unsigned flags) {
	assert(len >= 0);
	std::cout << "libuv recv len = " << len << std::endl;
	if (!len && !addr) {
		// no data
		utp_issue_deferred_acks(ctx.get());
	} else {
		size_t addrlen = addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
		if (!utp_process_udp(ctx.get(), static_cast<const byte *>(buf), len, addr, addrlen)) {
			// printf("UDP packet not handled by UTP.  Ignoring.\n");
		}
	}
}

uint64 UTPContext::sendTo(const void *buf, size_t len, const struct sockaddr *addr, socklen_t addrlen) {
	unique_ptr<char[]> tmpbuf(new char[len]);
	memcpy(tmpbuf.get(), buf, len);
	uv_buf_t uvbuf;
	uvbuf.base = tmpbuf.get();
	uvbuf.len = len;
	assert(uv_udp_send(new uv_udp_send_t, &udpHandle, &uvbuf, 1, addr, [] (uv_udp_send_t *req, int status) {
		delete req;
	}) >= 0);
	return 0;
}

bool UTPContext::onFirewall() {
	if (state != STATE_BOUND || !listening) return true; // not a listen socket
	if (backlog > 0 && pendingConnections >= backlog) return true; // pending connections reach limit
	return false;
}

void UTPContext::onAccept(utp_socket *sock) {
	assert(state == STATE_BOUND && listening);
	Nan::HandleScope scope;
	UTPSocket *utpsock = new UTPSocket(this, sock);
	v8::Local<v8::Object> connObj = utpsock->handle();
	v8::Local<v8::Function> onConn = Nan::Get(handle(), Nan::New("_onConnection").ToLocalChecked()).ToLocalChecked().As<v8::Function>();
	v8::Local<v8::Value> argv[1] = {connObj};
	Nan::Callback(onConn).Call(1, argv);
}

void UTPContext::Init(v8::Local<v8::Object> exports, v8::Local<v8::Object> module) {
	Nan::HandleScope scope;

	v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(New);
	tpl->SetClassName(Nan::New("UTPContext").ToLocalChecked());
	tpl->InstanceTemplate()->SetInternalFieldCount(1);

	Nan::SetPrototypeMethod(tpl, "bind", Bind);
	Nan::SetPrototypeMethod(tpl, "listen", Listen);
	Nan::SetPrototypeMethod(tpl, "connect", Connect);
	Nan::SetPrototypeMethod(tpl, "close", Close);
	Nan::SetPrototypeMethod(tpl, "state", State);
	Nan::SetPrototypeMethod(tpl, "ref", jsRef);
	Nan::SetPrototypeMethod(tpl, "unref", jsUnref);

	exports->Set(Nan::New("UTPContext").ToLocalChecked(), tpl->GetFunction());
	constructor.Reset(tpl->GetFunction());
}

NAN_METHOD(UTPContext::New) {
	Nan::HandleScope scope;
	UTPContext *utpctx = new UTPContext();
	utpctx->Wrap(info.This());
	info.GetReturnValue().Set(info.This());
}

NAN_METHOD(UTPContext::State) {
	Nan::HandleScope scope;
	UTPContext *utpctx = Nan::ObjectWrap::Unwrap<UTPContext>(info.Holder());
	info.GetReturnValue().Set(Nan::New(statestr[utpctx->state]).ToLocalChecked());
}

NAN_METHOD(UTPContext::Bind) {
	Nan::HandleScope scope;
	UTPContext *utpctx = Nan::ObjectWrap::Unwrap<UTPContext>(info.Holder());
	unsigned int port = Nan::To<v8::Uint32>(info[0]).ToLocalChecked()->Value();
	string host(*Nan::Utf8String(info[1]));
	assert(utpctx->state == STATE_INIT);
	int result = utpctx->bind(static_cast<uint16_t>(port), host);
	if (result == 0) {
		info.GetReturnValue().Set(Nan::New<v8::Boolean>(true));
	} else {
		v8::Local<v8::Value> err = Nan::Error(uv_strerror(result));
		Nan::To<v8::Object>(err).ToLocalChecked()->Set(Nan::New("code").ToLocalChecked(), Nan::New(uv_err_name(result)).ToLocalChecked());
		Nan::ThrowError(err);
	}
}

NAN_METHOD(UTPContext::Listen) {
	Nan::HandleScope scope;
	UTPContext *utpctx = Nan::ObjectWrap::Unwrap<UTPContext>(info.Holder());
	assert(utpctx->state == STATE_BOUND);
	int backlog = Nan::To<v8::Int32>(info[0]).ToLocalChecked()->Value();
	utpctx->listen(backlog);
}

NAN_METHOD(UTPContext::Connect) {
	Nan::HandleScope scope;
	UTPContext *utpctx = Nan::ObjectWrap::Unwrap<UTPContext>(info.Holder());
	unsigned int port = Nan::To<v8::Uint32>(info[0]).ToLocalChecked()->Value();
	string host(*Nan::Utf8String(info[1]));
	assert(utpctx->state == STATE_BOUND);
	UTPSocket *utpsock;
	int result = utpctx->connect(static_cast<uint16_t>(port), host, &utpsock);
	if (result == 0) {
		info.GetReturnValue().Set(utpsock->handle());
	} else {
		v8::Local<v8::Value> err = Nan::Error(uv_strerror(result));
		Nan::To<v8::Object>(err).ToLocalChecked()->Set(Nan::New("code").ToLocalChecked(), Nan::New(uv_err_name(result)).ToLocalChecked());
		Nan::ThrowError(err);
	}
}

NAN_METHOD(UTPContext::Close) {
	Nan::HandleScope scope;
	UTPContext *utpctx = Nan::ObjectWrap::Unwrap<UTPContext>(info.Holder());
	utpctx->destroy();
}

NAN_METHOD(UTPContext::jsRef) {
	Nan::HandleScope scope;
	UTPContext *utpctx = Nan::ObjectWrap::Unwrap<UTPContext>(info.Holder());
	utpctx->uvRef();
}

NAN_METHOD(UTPContext::jsUnref) {
	Nan::HandleScope scope;
	UTPContext *utpctx = Nan::ObjectWrap::Unwrap<UTPContext>(info.Holder());
	utpctx->uvUnref();
}

}
