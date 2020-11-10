#include <fstream>
#include <iostream>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <functional>

using namespace std;

using u32 = uint32_t;
using u16 = uint16_t;
using u8 = uint8_t;
using i32 = int32_t;
using bytes = vector<u8>;
const auto FORMAT_RED = "\033[31m";
const auto FORMAT_NIL = "\033[0m";
const i32 ENTRY_SIZE = 32;
const u16 CLUSTER_LIMIT = 0x0ff7;
const u8 ENTRY_DIRECTORY = 1u << 4u;

extern "C" auto my_print (const char *, bool err) -> void;

auto my_print (const string &s, bool err = false) -> void { my_print(s.c_str(), err); }

auto my_println (const string &s, bool err = false) -> void { my_print(s + "\n", err); }

auto my_println () -> void { my_println("", false); }

inline auto b2u (const bytes &bs, i32 offset, i32 n = 1) -> u32 {
    auto r = 0u;
    for (i32 i = n - 1; i >= 0; i--) r = ( u32 ) bs[ offset + i ] + ( r << 8u );
    return r;
}

inline auto split (const string &s, char by = ' ') -> vector<string> {
    vector<string> r;
    istringstream iss(s);
    while (true) {
        string word;
        getline(iss, word, by);
        if (!iss) break;
        if (word.empty()) continue;
        r.push_back(word);
    }
    return r;
}

inline auto read2vec (istream &from, bytes &to, i32 n) -> void {
    to.resize(n);
    for (auto &b: to) b = from.get();
}

enum Option { l };

class BPB {
public:
    u32 n_byte;
    u32 n_fat;
    u32 n_entry;
    u32 n_sector;
    u32 sector_per_fat;

    explicit BPB (const bytes &buf) :
        n_byte(b2u(buf, 11, 2)),
        n_fat(b2u(buf, 16)),
        n_entry(b2u(buf, 17, 2)),
        n_sector(b2u(buf, 19, 2)),
        sector_per_fat(b2u(buf, 22, 2)) {}
};

class Entry {
public:
    string name;
    string ext;
    u8 flags;
    u16 low16bit;
    u32 size;
    bool ok;

    explicit Entry (const bytes &buf) :
        name(string(buf.cbegin() + 0, buf.cbegin() + 8)),
        ext(string(buf.cbegin() + 8, buf.cbegin() + 11)),
        flags(b2u(buf, 11)),
        low16bit(b2u(buf, 26, 2)),
        size(b2u(buf, 28, 4)),
        ok(low16bit != 0u) {
        if (not ok) return;
        name.erase(find(name.begin(), name.end(), ' '), name.end());
        ext.erase(find(ext.begin(), ext.end(), ' '), ext.end());
    }

};

class Item {
public:
    string name;

    explicit Item (string name) :
        name(std::move(name)) {}

    virtual auto tree (const string &cwd, bool long_format) -> void {};

    virtual auto is_dir () -> bool { return false; }

    virtual auto is_file () -> bool { return false; }

    virtual auto detail () -> string { return ""; };

    virtual auto to_str () -> string = 0;

};

class File : public Item {
public:
    bytes content;
    u32 size;
    string ext;

    explicit File (string name, string ext, bytes content, u32 size) :
        Item(std::move(name)),
        content(std::move(content)),
        ext(std::move(ext)),
        size(size) {}

    auto to_str () -> string override {
        return name + "." + ext;
    }

    auto is_file () -> bool override { return true; }

    auto detail () -> string override {
        return to_string(size);
    }

    auto cat () -> string {
        string r;
        for (char c:content) {
            if (not c) break;
            r.push_back(c);
        }
        return r;
    }
};

class Dir : public Item {
    unordered_map<string, Item *> children;

public:
    explicit Dir (string name, Dir *parent = nullptr) : Item(std::move(name)) {
        if (parent) {
            children[ "." ] = this;
            children[ ".." ] = parent;
        }
    }

    auto add (Item *item) -> void {
        auto name = item->name;
        if (item->is_file()) name += "." + (( File * ) item )->ext;
        children[ name ] = item;
    }

    auto tree (const string &cwd, bool long_format) -> void override {
        my_print(cwd.c_str());
        if (long_format) my_print(" " + detail());
        my_print(":\n");

        for (auto &[name, child]:children) {
            bool special = name == "." or name == "..";
            if (special) my_print(FORMAT_RED + name + FORMAT_NIL);
            else my_print(child->to_str());
            my_print("  ");
            if (long_format) {
                if (special) my_println();
                else my_println(child->detail());
            }
        }
        my_println();

        for (auto &[name, child]:children) {
            if (name == "." or name == "..") continue;
            child->tree(cwd + child->name + "/", long_format);
        }
    }

    auto to_str () -> string override {
        return "\033[31m" + name + "\033[0m";
    }

    auto sub_dir (const string &name) -> Dir * {
        if (children.contains(name)) {
            auto child = children[ name ];
            if (child->is_dir())
                return ( Dir * ) child;
        }
        return nullptr;
    }

    auto sub_file (const string &name) -> File * {
        if (children.contains(name)) {
            auto child = children[ name ];
            if (child->is_file()) return ( File * ) child;
        }
        return nullptr;
    }

    auto is_dir () -> bool override { return true; }

    auto detail () -> string override {
        u32 dir_cnt = 0, file_cnt = 0;
        for (auto &[name, child]:children) {
            if (name == "." or name == "..") continue;
            if (child->is_file()) file_cnt++;
            else if (child->is_dir()) dir_cnt++;
        }
        return to_string(dir_cnt) + " " + to_string(file_cnt);
    }
};

class FileSystem {
    using Opts = unordered_set<Option>;
    using Func = function<void (FileSystem *, const vector<string> &, const Opts &)>;
    vector<u16> addrs;
    vector<bytes> data;

    auto content (const Entry &e) -> bytes {
        bytes bs;
        u16 cur;
        for (cur = e.low16bit; cur < CLUSTER_LIMIT; cur = addrs[ cur ]) {
            for (i32 i = 0; i < 512; ++i) {
                bs.push_back(data[ cur - 2 ][ i ]);
            }
        }
        if (cur == CLUSTER_LIMIT) {
            my_println("Bad Cluster!", true);
            exit(1);
        }
        return bs;
    }

    // ls [-l] [path=/]
    static auto ls (FileSystem *fs, const vector<string> &args, const Opts &options) -> void {
        auto path = args.empty() ? "/" : args[ 0 ];
        if (path.back() != '/') path.push_back('/');
        auto steps = split(path, '/');
        auto cur = fs->root;
        for (auto step:steps) {
            cur = cur->sub_dir(step);
            if (cur == nullptr) {
                my_println("Directory not existed!", true);
                return;
            }
        }
        cur->tree(path, options.contains(Option::l));
    }

    // cat <path>
    static auto cat (FileSystem *fs, const vector<string> &args, const Opts &options) -> void {
        if (args.empty()) {
            my_println("Need an argument: <path>", true);
            return;
        }
        if (!options.empty()) {
            my_println("unexpected option detected.", true);
            return;
        }
        auto steps = split(args[ 0 ], '/');
        if (steps.empty()) {
            my_println("Specify a file to cat please.");
            return;
        }

        auto file_name = steps.back();
        steps.pop_back();
        auto cur = fs->root;
        for (auto step:steps) {
            cur = cur->sub_dir(step);
            if (not cur) {
                my_println("Directory not existed: " + args[ 0 ], true);
                return;
            }
        }
        auto file = cur->sub_file(file_name);
        if (not file) {
            my_println("File not existed: " + args[ 0 ], true);
            return;
        }
        my_println(file->cat());
    }

    Dir *root = new Dir("root");

    auto add (const Entry &e, Dir *dir) -> void {
        auto all = content(e);
        if (e.flags & ENTRY_DIRECTORY) {
            auto sub_dir = new Dir(e.name, dir);
            bytes tmp(ENTRY_SIZE);
            for (i32 i = 0; i < all.size() / ENTRY_SIZE; ++i) {
                for (i32 j = 0; j < ENTRY_SIZE; ++j) {
                    tmp[ j ] = all[ i * ENTRY_SIZE + j ];
                }
                Entry sub_entry(tmp);

                if (sub_entry.ok and
                    sub_entry.name != "." and
                    sub_entry.name != "..") {
                    add(sub_entry, sub_dir);
                }
            }
            dir->add(sub_dir);
        } else {
            dir->add(new File(e.name, e.ext, all, e.size));
        }
    }

    auto add (const Entry &e) -> void {
        add(e, root);
    }

public:

    [[noreturn]] auto loop () -> void {

        unordered_map<string, Func> call_map {
            { "ls",  this->ls },
            { "cat", this->cat },
        };
        unordered_map<char, Option> opt_map {
            { 'l', Option::l },
        };

        repl:
        while (true) {
            if (cin.eof()) {
                my_println("bye~\n");
                exit(0);
            }
            my_println("----------------------------------------");
            my_print("> ");
            string line;
            getline(cin, line);
            auto words = split(line);
            if (words.empty()) goto repl;
            string cmd = words[ 0 ];
            vector<string> args;
            Opts options;
            if (not call_map.contains(cmd)) {
                my_println("Unexpected command: " + cmd, true);
                goto repl;
            }
            for (int i = 1; i < words.size(); ++i) {
                auto &word = words[ i ];
                if (word[ 0 ] == '-') {
                    for (int j = 1; j < word.size(); ++j) {
                        if (not opt_map.contains(word[ j ])) {
                            string hint("Unexpected option: ");
                            hint.push_back(word[ j ]);
                            my_println(hint, true);
                            goto repl;
                        }
                        options.insert(opt_map[ word[ j ]]);
                    }
                } else {
                    args.push_back(word);
                }
            }
            call_map[ cmd ](this, args, options);
        }
    }

    FileSystem (const string &path) {
        fstream img(path, ios::binary | ios::in);
        if (not img) {
            my_println("failed to open!", true);
            exit(1);
        }
        bytes bpb_buf;
        read2vec(img, bpb_buf, 512);
        BPB bpb(bpb_buf);

        bytes fat;
        for (i32 i = 0; i < bpb.n_fat; ++i) {
            read2vec(img, fat, bpb.n_byte * bpb.sector_per_fat);
        }
        for (i32 i = 0; i < bpb.n_byte * bpb.sector_per_fat / 3; ++i) {
            u32 lower = b2u(fat, i * 3 + 0);
            u32 higher = ( u8 ) b2u(fat, i * 3 + 1) & 0x0fu;
            lower = lower | higher << 8u;
            addrs.push_back(lower);

            lower = ( u8 ) b2u(fat, i * 3 + 1) & 0xf0u;
            lower >>= 4u;
            higher = b2u(fat, i * 3 + 2);
            lower |= higher << 4u;
            addrs.push_back(lower);
        }

        vector<bytes> entry_buffer(bpb.n_entry);
        for (auto &buf: entry_buffer) read2vec(img, buf, ENTRY_SIZE);

        auto padding = -bpb.n_entry * ENTRY_SIZE % bpb.n_byte;
        img.seekg(padding, ios::cur);

        auto data_sector_number = ( bpb.n_sector * bpb.n_byte - img.tellg()) / bpb.n_byte;
        bytes cluster(bpb.n_byte);
        for (i32 i = 0; i < data_sector_number; ++i) {
            read2vec(img, cluster, bpb.n_byte);
            data.push_back(cluster);
        }

        for (auto eb:entry_buffer) {
            Entry e(eb);
            if (e.ok) add(e);
        }

        my_println("Init finished." "\n" "Hello, FAT-12.");
    }
};

auto main () -> i32 {
    FileSystem fs("a.img");
    fs.loop();
}
