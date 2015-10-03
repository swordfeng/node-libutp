{
	"targets": [
		{
			"target_name": "node-libutp",
			'dependencies': [
				'libutp',
			],
			'include_dirs': [
				'deps/libutp',
				'<!(node -e "require(\'nan\')")',
			],
			"sources": [
				'src/utp_context.cc',
				'src/utp_socket.cc',
				'src/utp.cc'
			],
			'defines':[
				'UTP_DEBUG_LOGGING'
			],
			'conditions': [
				['OS=="win"', {
					'libraries': [
						'Ws2_32.lib'
					],
				}, { # OS!="win"
					'cflags': [
						'-std=c++11',
						'-fno-exceptions',
						'-g'
					],
					'defines':[
						'POSIX'
					],
					'libraries': [
					],
				}],
			]
		},
		{
			'target_name': 'libutp',
			'type': 'static_library',
			'defines': [
			    'UTP_DEBUG_LOGGING'
			],
			'sources': [
				'deps/libutp/utp_callbacks.cpp',
				'deps/libutp/utp_internal.cpp',
				'deps/libutp/utp_utils.cpp',
				'deps/libutp/utp_api.cpp',
				'deps/libutp/utp_hash.cpp',
				'deps/libutp/utp_packedsockaddr.cpp'
			],
			'conditions': [
				['OS=="linux" or OS=="android"', {
					'cxxflags': [
						'-Wall',
						'-fno-exceptions',
						'-fPIC',
						'-fno-rtti',
						'-Wno-sign-compare',
						'-fpermissive',
						'-g'
					],
					'defines' :[
						'POSIX',
					]
				}],
				['OS=="win"', {
					'defines': [
						'WIN32',
						'ENABLE_I18N',
						'ENABLE_SRP=1'
					],
					'sources':[
						'deps/libutp/libutp_inet_ntop.cpp',
					],
					'libraries': [
						'Ws2_32.lib'
					],
				}, {  # OS != "win",
				}],
			],
		},
	],
}
