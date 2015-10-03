node-libutp
===
libutp bindings for node.js
API is similiar to the internal 'net' module.

socket#pause() and socket#resume() are not implemented.
(I cannot find a way to implement them, if you know please tell me!)

The library may not suitable for using in production.
If you find any bugs, feel free to open an issue.

TODO: use epoll instead of libuv on linux so that ICMP messages can be handled.
