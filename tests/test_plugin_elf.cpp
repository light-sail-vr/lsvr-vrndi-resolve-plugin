// ELF-hygiene smoke test (Linux; the counterpart of test_plugin_delayload).
// Resolve loads plugins into a process that ships its own C++ runtime and
// libraries under /opt/resolve/libs, so the .ofx must depend on as little of
// the host's world as possible (spec decision 6) and must load on the oldest
// glibc Blackmagic certifies (decision 5). Parses the ELF directly, so no
// binutils are needed where it runs (the Ubuntu load-check container), and
// asserts:
//   1. NEEDED is glibc pieces only: no libstdc++, libgcc_s, libz or
//      libcudart (linked statically) and no libndi.so.6, which the plugin
//      dlopens from its bundle at runtime (spec decision 8, amended by #71:
//      libndi itself NEEDs the Avahi client libraries, so a link-time import
//      would hide the plugin on machines without them)
//   2. no GLIBC_ symbol version above 2.28 (Rocky/RHEL 8)
//   3. no RUNPATH and no legacy DT_RPATH: nothing is resolved beside the
//      plugin by the loader any more
//   4. the only exported symbols are the two OFX entry points
//   5. the plugin dlopens with RTLD_NOW and OfxGetNumberOfPlugins() == 1
// For 5 the .ofx is copied ALONE into a temp directory: no libndi.so.6
// beside it, the case the runtime loading exists for.
//
// Usage: test_plugin_elf <NDIOutput.ofx>

#include "ofxCore.h"

#include <dlfcn.h>
#include <elf.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

static int failures = 0;

static void expectTrue(bool actual, const std::string& name)
{
    if (!actual) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name.c_str());
    } else {
        std::printf("ok   %s\n", name.c_str());
    }
}

static bool readFile(const std::string& path, std::vector<unsigned char>* out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static bool writeFile(const std::string& path, const std::vector<unsigned char>& data)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

// What the checks need from the dynamic section, symbol table and version
// requirements of a 64-bit little-endian ELF shared object.
struct ElfFacts {
    std::vector<std::string> needed;
    std::vector<std::string> runpath;   // DT_RUNPATH entries (normally one)
    std::vector<std::string> rpath;     // legacy DT_RPATH entries
    std::set<std::string> exported;     // defined, non-local, visible dynsyms
    std::set<std::string> glibcVersions; // GLIBC_* names from .gnu.version_r
};

static bool parseElf(const std::vector<unsigned char>& b, ElfFacts* out, std::string* err)
{
    if (b.size() < sizeof(Elf64_Ehdr) || std::memcmp(b.data(), ELFMAG, SELFMAG) != 0) {
        *err = "not an ELF file";
        return false;
    }
    if (b[EI_CLASS] != ELFCLASS64 || b[EI_DATA] != ELFDATA2LSB) {
        *err = "not a 64-bit little-endian ELF";
        return false;
    }
    Elf64_Ehdr eh;
    std::memcpy(&eh, b.data(), sizeof(eh));
    if (eh.e_shentsize != sizeof(Elf64_Shdr) ||
        eh.e_shoff + static_cast<Elf64_Off>(eh.e_shnum) * sizeof(Elf64_Shdr) > b.size()) {
        *err = "section header table out of range";
        return false;
    }
    std::vector<Elf64_Shdr> sh(eh.e_shnum);
    std::memcpy(sh.data(), b.data() + eh.e_shoff, eh.e_shnum * sizeof(Elf64_Shdr));

    auto inRange = [&](Elf64_Off off, Elf64_Xword len) {
        return off <= b.size() && len <= b.size() - off;
    };
    // NUL-terminated string at offset `idx` of section `strIndex`.
    auto str = [&](Elf64_Word strIndex, Elf64_Xword idx) -> std::string {
        if (strIndex >= sh.size()) return std::string();
        const Elf64_Shdr& s = sh[strIndex];
        if (idx >= s.sh_size || !inRange(s.sh_offset, s.sh_size)) return std::string();
        const char* p = reinterpret_cast<const char*>(b.data() + s.sh_offset + idx);
        return std::string(p, strnlen(p, s.sh_size - idx));
    };

    bool sawDynamic = false;
    for (const Elf64_Shdr& s : sh) {
        if (s.sh_type == SHT_NOBITS) continue; // .bss & co. occupy no file bytes
        if (!inRange(s.sh_offset, s.sh_size)) {
            *err = "section out of range";
            return false;
        }
        const unsigned char* base = b.data() + s.sh_offset;
        if (s.sh_type == SHT_DYNAMIC) {
            sawDynamic = true;
            for (Elf64_Xword i = 0; i + 1 <= s.sh_size / sizeof(Elf64_Dyn); ++i) {
                Elf64_Dyn d;
                std::memcpy(&d, base + i * sizeof(Elf64_Dyn), sizeof(d));
                if (d.d_tag == DT_NULL) break;
                if (d.d_tag == DT_NEEDED) out->needed.push_back(str(s.sh_link, d.d_un.d_val));
                if (d.d_tag == DT_RUNPATH) out->runpath.push_back(str(s.sh_link, d.d_un.d_val));
                if (d.d_tag == DT_RPATH) out->rpath.push_back(str(s.sh_link, d.d_un.d_val));
            }
        } else if (s.sh_type == SHT_DYNSYM) {
            for (Elf64_Xword i = 1; i < s.sh_size / sizeof(Elf64_Sym); ++i) {
                Elf64_Sym sym;
                std::memcpy(&sym, base + i * sizeof(Elf64_Sym), sizeof(sym));
                const unsigned bind = ELF64_ST_BIND(sym.st_info);
                const unsigned vis = ELF64_ST_VISIBILITY(sym.st_other);
                if (sym.st_shndx == SHN_UNDEF || bind == STB_LOCAL) continue;
                if (vis != STV_DEFAULT && vis != STV_PROTECTED) continue;
                out->exported.insert(str(s.sh_link, sym.st_name));
            }
        } else if (s.sh_type == SHT_GNU_verneed) {
            Elf64_Off off = 0;
            for (Elf64_Word n = 0; n < s.sh_info; ++n) {
                Elf64_Verneed vn;
                if (!inRange(s.sh_offset + off, sizeof(vn))) break;
                std::memcpy(&vn, base + off, sizeof(vn));
                Elf64_Off aoff = off + vn.vn_aux;
                for (Elf64_Half a = 0; a < vn.vn_cnt; ++a) {
                    Elf64_Vernaux va;
                    if (!inRange(s.sh_offset + aoff, sizeof(va))) break;
                    std::memcpy(&va, base + aoff, sizeof(va));
                    const std::string name = str(s.sh_link, va.vna_name);
                    if (name.compare(0, 6, "GLIBC_") == 0) out->glibcVersions.insert(name);
                    if (!va.vna_next) break;
                    aoff += va.vna_next;
                }
                if (!vn.vn_next) break;
                off += vn.vn_next;
            }
        }
    }
    if (!sawDynamic) {
        *err = "no .dynamic section (not a shared object?)";
        return false;
    }
    return true;
}

// "GLIBC_2.17" -> {2,17,0}. False for anything non-numeric (GLIBC_PRIVATE).
static bool parseGlibcVersion(const std::string& name, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    const char* p = name.c_str() + 6;
    for (int i = 0; i < 3; ++i) {
        char* end = nullptr;
        errno = 0;
        const long n = std::strtol(p, &end, 10);
        if (end == p || errno) return false;
        v[i] = static_cast<int>(n);
        if (*end == '\0') return true;
        if (*end != '.') return false;
        p = end + 1;
    }
    return false;
}

// The glibc pieces a plugin may link dynamically: they are the platform.
static bool isAllowedNeeded(const std::string& lib)
{
    static const char* const kAllowed[] = {
        "libc.so.6", "libm.so.6", "libdl.so.2",
        "libpthread.so.0", "librt.so.1",
    };
    for (const char* a : kAllowed) {
        if (lib == a) return true;
    }
    // The dynamic loader itself (ld-linux-x86-64.so.2; ld-linux-aarch64.so.1
    // on an arm64 dev container), pulled in by TLS or stack-protector refs.
    return lib.compare(0, 9, "ld-linux-") == 0;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_plugin_elf <NDIOutput.ofx>\n");
        return 2;
    }
    const std::string ofxPath = argv[1];

    std::vector<unsigned char> ofx;
    if (!readFile(ofxPath, &ofx)) {
        std::fprintf(stderr, "cannot read %s\n", ofxPath.c_str());
        return 2;
    }

    ElfFacts facts;
    std::string err;
    if (!parseElf(ofx, &facts, &err)) {
        std::fprintf(stderr, "FAIL %s: %s\n", ofxPath.c_str(), err.c_str());
        return 1;
    }

    // 1. NEEDED allowlist.
    bool sawNdi = false;
    for (const std::string& lib : facts.needed) {
        expectTrue(isAllowedNeeded(lib), "NEEDED " + lib + " is allowed");
        sawNdi = sawNdi || lib == "libndi.so.6";
    }
    expectTrue(!sawNdi, "NEEDED excludes libndi.so.6 (dlopened from the bundle at runtime)");

    // 2. glibc symbol-version ceiling.
    int maxV[3] = {0, 0, 0};
    std::string maxName = "(none)";
    for (const std::string& name : facts.glibcVersions) {
        int v[3];
        if (!parseGlibcVersion(name, v)) {
            expectTrue(false, "glibc version " + name + " is a numbered release");
            continue;
        }
        if (std::lexicographical_compare(maxV, maxV + 3, v, v + 3)) {
            std::memcpy(maxV, v, sizeof(maxV));
            maxName = name;
        }
    }
    const int ceiling[3] = {2, 28, 0};
    expectTrue(!std::lexicographical_compare(ceiling, ceiling + 3, maxV, maxV + 3),
               "highest glibc version " + maxName + " <= GLIBC_2.28");

    // 3. No library search path baked in.
    expectTrue(facts.runpath.empty(),
               "no RUNPATH (got " +
                   (facts.runpath.empty() ? std::string("none") : facts.runpath[0]) + ")");
    expectTrue(facts.rpath.empty(), "no legacy DT_RPATH");

    // 4. Exported-symbol allowlist.
    const std::set<std::string> wantExports = {"OfxGetNumberOfPlugins", "OfxGetPlugin"};
    for (const std::string& s : facts.exported) {
        if (!wantExports.count(s)) expectTrue(false, "unexpected export " + s);
    }
    for (const std::string& s : wantExports) {
        expectTrue(facts.exported.count(s) == 1, "exports " + s);
    }
    std::printf("     (%zu exported symbol(s))\n", facts.exported.size());

    // 5. dlopen from an isolated directory with no libndi.so.6 beside it.
    char tmpl[] = "/tmp/ndi_plugin_elf_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) {
        std::fprintf(stderr, "mkdtemp failed\n");
        return 2;
    }
    const std::string stagedOfx = std::string(dir) + "/NDIOutput.ofx";
    if (!writeFile(stagedOfx, ofx)) {
        std::fprintf(stderr, "cannot stage files in %s\n", dir);
        return 2;
    }
    void* h = dlopen(stagedOfx.c_str(), RTLD_NOW | RTLD_LOCAL);
    expectTrue(h != nullptr, std::string("dlopen(RTLD_NOW) succeeds without libndi.so.6") +
                                 (h ? "" : std::string(": ") + dlerror()));
    if (h) {
        using GetCount = int (*)();
        using GetPlugin = OfxPlugin* (*)(int);
        GetCount getCount = reinterpret_cast<GetCount>(dlsym(h, "OfxGetNumberOfPlugins"));
        GetPlugin getPlugin = reinterpret_cast<GetPlugin>(dlsym(h, "OfxGetPlugin"));
        expectTrue(getCount && getPlugin, "both entry points resolve via dlsym");
        if (getCount && getPlugin) {
            expectTrue(getCount() == 1, "OfxGetNumberOfPlugins() == 1");
            const OfxPlugin* p = getPlugin(0);
            expectTrue(p && p->pluginIdentifier &&
                           std::strcmp(p->pluginIdentifier, "LSVR.NDIOutput") == 0,
                       "OfxGetPlugin(0) is LSVR.NDIOutput");
        }
        // Deliberately no dlclose: process exit is the host's unload model.
    }
    unlink(stagedOfx.c_str());
    rmdir(dir);

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all plugin ELF checks passed\n");
    return 0;
}
