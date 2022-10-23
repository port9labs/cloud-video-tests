/*
MIT License

Copyright (c) 2022 Port 9 Labs

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "protocol.h"
#include <netdb.h>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>

UDPAddress::UDPAddress(std::string hostname, std::string port, int offset)
{
    struct addrinfo hints {
    };
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_ADDRCONFIG;
    if (offset != 0) {
        auto iport = std::stoi(port) + offset;
        port = std::to_string(iport);
    }
    auto err = getaddrinfo(hostname.c_str(), port.c_str(), &hints, &m_addrinfo);
    if (err != 0) { throw std::invalid_argument(gai_strerror(err)); }
}

int UDPAddress::connect(int fd) { return ::connect(fd, m_addrinfo->ai_addr, m_addrinfo->ai_addrlen); }

UDPAddress::~UDPAddress()
{
    if (m_addrinfo != nullptr) freeaddrinfo(m_addrinfo);
}

int UDPAddress::bind(int fd) {
    auto err = ::bind(fd, m_addrinfo->ai_addr, m_addrinfo->ai_addrlen);
    //if (err != 0) { throw std::invalid_argument(strerror(errno)); }
    return err;
}
