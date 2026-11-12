// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

/*
    Somewhere Link on Windows: advertises Elsewhere as "_somewhere._tcp" on the local network, so
    phones find it the same way they find a Mac. Uses Windows' own DNS-SD (windns.h, Windows 10
    1809 and later), so nothing extra needs installing. Compiled on Windows only.
*/

#ifdef _WIN32

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <windns.h>
#include <string>

#pragma comment(lib, "dnsapi.lib")

namespace elsewhere::somewherelink
{
    namespace
    {
        struct Registration
        {
            DNS_SERVICE_REGISTER_REQUEST request {};
            PDNS_SERVICE_INSTANCE instance = nullptr;
            std::wstring service, host, project;
        };

        void WINAPI registered (DWORD status, PVOID, PDNS_SERVICE_INSTANCE instance)
        {
            if (status != ERROR_SUCCESS)
                OutputDebugStringA ("Somewhere Link: DNS-SD registration failed\n");
            if (instance != nullptr)
                DnsServiceFreeInstance (instance);
        }

        std::wstring widen (const std::string& utf8)
        {
            if (utf8.empty()) return {};
            const int n = MultiByteToWideChar (CP_UTF8, 0, utf8.data(), (int) utf8.size(), nullptr, 0);
            std::wstring w ((size_t) n, L'\0');
            MultiByteToWideChar (CP_UTF8, 0, utf8.data(), (int) utf8.size(), w.data(), n);
            return w;
        }
    }

    /** Starts advertising; returns a handle for unadvertise, or null if Windows can't (before 1809). */
    void* advertise (int port, const std::string& projectName)
    {
        wchar_t computer[256] = {};
        DWORD size = 256;
        if (! GetComputerNameExW (ComputerNameDnsHostname, computer, &size))
            return nullptr;

        auto* r = new Registration();
        r->service = std::wstring (computer) + L"._somewhere._tcp.local";
        r->host = std::wstring (computer) + L".local";
        r->project = widen (projectName.substr (0, 200));

        PCWSTR keys[] = { L"project", L"v" };
        PCWSTR values[] = { r->project.c_str(), L"1" };
        r->instance = DnsServiceConstructInstance (r->service.c_str(), r->host.c_str(), nullptr, nullptr,
                                                   (WORD) port, 0, 0, 2, keys, values);
        if (r->instance == nullptr) { delete r; return nullptr; }

        r->request.Version = DNS_QUERY_REQUEST_VERSION1;
        r->request.InterfaceIndex = 0;
        r->request.pServiceInstance = r->instance;
        r->request.pRegisterCompletionCallback = registered;
        r->request.pQueryContext = r;
        r->request.unicastEnabled = FALSE;

        if (DnsServiceRegister (&r->request, nullptr) != DNS_REQUEST_PENDING)
        {
            DnsServiceFreeInstance (r->instance);
            delete r;
            return nullptr;
        }
        return r;
    }

    void unadvertise (void* handle)
    {
        auto* r = static_cast<Registration*> (handle);
        if (r == nullptr) return;
        DnsServiceDeRegister (&r->request, nullptr);
        DnsServiceFreeInstance (r->instance);
        delete r;
    }
}

#endif
