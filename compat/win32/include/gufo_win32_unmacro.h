/* <windows.h> defines macros that collide with ordinary C++ identifiers in
 * Gufo (enumerators such as DELETE or ERROR, members such as SendMessage).
 * Include this after any Win32 header. Only names that break Gufo belong
 * here: removing a macro another Win32 header still needs breaks that header. */
#undef DELETE
#undef ERROR
#undef IN
#undef OUT
#undef OPTIONAL
#undef interface
#undef near
#undef far
/* Winsock's FD_* macros expand FAR after this cleanup. Flat Windows targets
 * have no far pointers, but FAR must not expand to the now-unbound `far`. */
#undef FAR
#define FAR
#undef small
#undef min
#undef max
#undef SendMessage
#undef PostMessage
#undef GetMessage
#undef GetObject
#undef LoadImage
#undef DrawText
#undef GetCurrentTime
#undef Yield
