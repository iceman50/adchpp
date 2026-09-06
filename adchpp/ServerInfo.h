/*
 * Copyright (C) 2006-2025 Jacek Sieka, arnetheduck on gmail point com
 * Copyright (C) 2026 iceman50
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef ADCHPP_SERVER_INFO_H
#define ADCHPP_SERVER_INFO_H

// TLS minimum protocol version constants from OpenSSL:
// https://github.com/openssl/openssl/blob/openssl-3.0.0/include/openssl/prov_ssl.h#L23-L26
// https://www.openssl.org/docs/man1.1.1/man3/SSL_CTX_set_min_proto_version.html

#define TLS1_VERSION	0x0301
#define TLS1_1_VERSION	0x0302
#define TLS1_2_VERSION	0x0303
#define TLS1_3_VERSION	0x0304

namespace adchpp {

struct ServerInfo {
	// BindAddress remains supported for existing configurations. The
	// family-specific values allow one logical listener to create independent
	// IPv4 and IPv6 sockets with IPV6_V6ONLY enabled.
	std::string ip;
	std::string bind4;
	std::string bind6;
	std::string address4;
	std::string address6;
	std::string port;

	struct TLSInfo {
		std::string cert;
		std::string pkey;
		std::string trustedPath;
		std::string dh;

		std::string cipherSuites13;
		int minVersion = TLS1_2_VERSION; // TLS 1.2+ by default
		int securityLevel = 1; // minimum of 80 bits of security
	private:
		friend struct ServerInfo;
		bool secure() const {
			return !cert.empty() && !pkey.empty() && !trustedPath.empty() && !dh.empty();
		}
	} TLSParams;
	bool secure() const { return TLSParams.secure(); }
};

}

#endif
