
solution "proxy"
	platforms { "portable", "x64", "avx", "avx2" }
	configurations { "Debug", "Release" }
	targetdir "bin/"
	rtti "Off"
	warnings "Extra"
	floatingpoint "Fast"
	flags { "FatalWarnings" }
	filter "configurations:Debug"
		symbols "On"
		defines { "_DEBUG" }
	filter "configurations:Release"
		optimize "Speed"
		defines { "NDEBUG" }
		editandcontinue "Off"
	filter "platforms:*x64 or *avx or *avx2"
		architecture "x86_64"

project "proxy"
	kind "ConsoleApp"
	links { "sodium" }
	files {
		"proxy.h",
		"proxy.cpp"
		--"proxy_*.h",
		--"proxy_*.cpp"
	}
	filter "system:not windows"
		links { "pthread" }
	filter "system:macosx"
		linkoptions { "-framework SystemConfiguration -framework CoreFoundation" }
