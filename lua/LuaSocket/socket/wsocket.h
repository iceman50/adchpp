#ifndef WSOCKET_H
#define WSOCKET_H
/*=========================================================================*\
* Socket compatibilization module for Win32
* LuaSocket toolkit
*
* RCS ID: $Id: wsocket.h,v 1.4 2005/10/07 04:40:59 diego Exp $
\*=========================================================================*/

/*=========================================================================*\
* WinSock include files
\*=========================================================================*/
#include <winsock.h>

// This def is moved from a header that's sharing data types between winsock and winsock2 
// into windsock2 headers in recent mingw-w64 versions.
// See https://sourceforge.net/p/mingw-w64/mailman/mingw-w64-public/thread/CALK-3m+zh=dO3zCiR6EqvE5YsJPwjuxnWvL_YfipOGHU32jTXQ@mail.gmail.com/
// The new headers included in proper order still give warnings about including
// various other headers such as windows.h in wrong order 
// so let's just play safe and include only what's missing...
 
#ifdef __MINGW64_VERSION_MAJOR
	#if __MINGW64_VERSION_MAJOR > 8
	typedef struct ip_mreq {
		struct in_addr imr_multiaddr;
		struct in_addr imr_interface;
	} IP_MREQ, *PIP_MREQ; 
	#endif
#endif

typedef int socklen_t;
typedef SOCKET t_socket;
typedef t_socket *p_socket;

#define SOCKET_INVALID (INVALID_SOCKET)

#endif /* WSOCKET_H */
