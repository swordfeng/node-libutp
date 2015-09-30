{
	"targets": [
		{
			"target_name": "node-libutp",
			'dependencies': [
				'libutp',
			],
			'include_dirs': [
				'libutp/',
			],
			"sources": [
			],
			'conditions': [
				['OS=="win"', {
					'libraries': [
					],
				}, { # OS!="win"
					'cflags': [
						'-std=c++11',
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
			    'POSIX',
			    '_DEBUG',
			    'UTP_DEBUG_LOGGING'
			],
			'sources': [
				'libutp/utp_callbacks.cpp',
				'libutp/utp_internal.cpp',
				'libutp/utp_utils.cpp',
				'libutp/utp_api.cpp',
				'libutp/utp_hash.cpp',
				'libutp/utp_packedsockaddr.cpp'
			],
			'conditions': [
				['OS=="linux" or OS=="android"', {
					'cxxflags': [
						'-Wall',
						'-fno-exceptions',
						'-fPIC',
						'-fno-rtti',
						'-Wno-sign-compare',
						'-fpermissive'
					],
				}],
				['OS=="win"', {
					'defines': [
						'WIN32',
						'ENABLE_I18N',
						'ENABLE_SRP=1'
					],
				}, {  # OS != "win",
				}],
			],
		},
	],
}
