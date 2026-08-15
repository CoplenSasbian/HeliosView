# make_bridge_inc.cmake — wrap a text file in a C++ raw-string literal so a
# translation unit can embed it with #include.
#
# Usage:
#   cmake -DIN=<input file> -DOUT=<output .inc> -DVAR=<variable name> -P make_bridge_inc.cmake
#
# The output declares:
#   static const char* <VAR> = R"JS(<file contents>)JS";
#
# The raw-string delimiter )JS" must not occur in the input file (the bridge
# shim is plain ES5, so it never does). The file is re-read verbatim, so the
# .inc is regenerated whenever the input changes.

file(READ "${IN}" _content)
file(WRITE "${OUT}"
"/* Generated from ${IN} by make_bridge_inc.cmake — do not edit. */\n"
"static const char* ${VAR} =\n"
"R\"JS(${_content})JS\";\n")
