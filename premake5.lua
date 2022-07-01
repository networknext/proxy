
solution "proxy"
	platforms { "portable", "x64", "avx", "avx2" }
	configurations { "Debug", "Release" }
	targetdir "bin/"
	rtti "Off"
	warnings "Extra"
	floatingpoint "Fast"
	flags { "FatalWarnings" }
	defines { "NEXT_COMPILE_WITH_TESTS", "NEXT_DEVELOPMENT=1", "NEXT_DISABLE_ADVANCED_PACKET_FILTER=1" }
	filter "configurations:Debug"
		symbols "On"
		defines { "_DEBUG" }
	filter "configurations:Release"
		optimize "Speed"
		defines { "NDEBUG" }
		editandcontinue "Off"
	filter "platforms:*x64 or *avx or *avx2"
		architecture "x86_64"

project "next"
	kind "StaticLib"
	links { "sodium" }
	files {
		"next.h",
		"next.cpp",
		"next_*.h",
		"next_*.cpp"
	}
	filter "system:not windows"
		links { "pthread" }
	filter "system:macosx"
		linkoptions { "-framework SystemConfiguration -framework CoreFoundation" }

project "proxy"
	kind "ConsoleApp"
	links { "sodium", "next" }
	files {
		"proxy.h",
		"proxy.cpp",
		"proxy_*.h",
		"proxy_*.cpp"
	}
	filter "system:not windows"
		links { "pthread" }
	filter "system:macosx"
		linkoptions { "-framework SystemConfiguration -framework CoreFoundation" }

project "client"
	kind "ConsoleApp"
	links { "next", "sodium" }
	files {
		"client.cpp"
	}
	filter "system:not windows"
		links { "pthread" }
	filter "system:macosx"
		linkoptions { "-framework SystemConfiguration -framework CoreFoundation" }
