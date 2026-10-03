# Turns a file into a C++ source with its bytes, so it can be built into Cemu.
#   cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DNAMESPACE=<ns> -DSYMBOL=<name> -P EmbedFile.cmake
# defines  const uint8_t* <ns>::<name>Data()  and  size_t <ns>::<name>Size()
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hexLength)
math(EXPR size "${hexLength} / 2")
# 32 bytes per line (some compilers dislike very long lines)
set(bytes "")
set(offset 0)
while(offset LESS hexLength)
	string(SUBSTRING "${hex}" ${offset} 64 chunk)
	string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," chunk "${chunk}")
	string(APPEND bytes "${chunk}\n")
	math(EXPR offset "${offset} + 64")
endwhile()
get_filename_component(inputName "${INPUT}" NAME)
file(WRITE "${OUTPUT}.tmp"
"// Generated from ${inputName} by cmake/EmbedFile.cmake. Do not edit.
#include <cstddef>
#include <cstdint>

namespace ${NAMESPACE}
{
	namespace
	{
		alignas(16) const uint8_t kData[${size} + 1] = {
${bytes}
0};
	}

	const uint8_t* ${SYMBOL}Data()
	{
		return kData;
	}

	size_t ${SYMBOL}Size()
	{
		return ${size};
	}
}
")
# only touch the output when it changed, so an unchanged video does not recompile
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
