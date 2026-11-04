// Native test driver for the D3D9 -> GLSL translator: translates every shader dump in a directory.
//   xlate_test <dir-with-.bin> [out-dir]
#include "../gfx/shader_xlate.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
int main(int argc, char **argv)
{
    if (argc < 2) return 1;
    std::string dir = argv[1], out = argc > 2 ? argv[2] : "";
    std::vector<std::string> files;
    DIR *d = opendir(dir.c_str());
    while (dirent *e = readdir(d)) { std::string n = e->d_name; if (n.size() > 4 && n.substr(n.size() - 4) == ".bin") files.push_back(n); }
    closedir(d);
    std::sort(files.begin(), files.end());
    int ok = 0, bad = 0, skip = 0;
    std::map<std::string, int> errs;
    for (auto &f : files) {
        FILE *fp = fopen((dir + "/" + f).c_str(), "rb");
        std::vector<uint32_t> w; uint32_t x;
        while (fread(&x, 4, 1, fp) == 1) w.push_back(x);
        fclose(fp);
        if ((w[0] & 0xFFFF) < 0x200) { ++skip; continue; }
        std::string glsl, err; d3dxlate::Info info;
        d3dxlate::Options opt;
        if (d3dxlate::Translate(w.data(), w.size(), opt, glsl, info, err)) {
            ++ok;
            if (!out.empty()) { FILE *o = fopen((out + "/" + f.substr(0, f.size() - 4) + (info.isPixel ? ".frag" : ".vert")).c_str(), "wb"); fwrite(glsl.data(), 1, glsl.size(), o); fclose(o); }
        } else { ++bad; errs[err]++; fprintf(stderr, "FAIL %s: %s\n", f.c_str(), err.c_str()); }
    }
    printf("translated %d, failed %d, skipped(SM1) %d\n", ok, bad, skip);
    for (auto &kv : errs) printf("  %5d x %s\n", kv.second, kv.first.c_str());
    return bad ? 2 : 0;
}
