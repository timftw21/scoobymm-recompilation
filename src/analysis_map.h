#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace analysis_map {

using Word = std::uint32_t;
using Path = std::filesystem::path;

inline std::string hex(Word value) { return std::format("0x{:08X}", value); }

inline Word number(std::string_view text, int base = 10) {
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    Word value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("Invalid number: " + std::string(text));
    }
    return value;
}

template<std::size_t N>
std::array<std::string_view, N> fields(std::string_view row) {
    std::array<std::string_view, N> result;
    for (std::size_t i = 0; i < N - 1; ++i) {
        const auto tab = row.find('\t');
        if (tab == std::string_view::npos) throw std::runtime_error("Truncated TSV row");
        result[i] = row.substr(0, tab);
        row.remove_prefix(tab + 1);
    }
    if (row.contains('\t')) throw std::runtime_error("Unexpected TSV column");
    result.back() = row;
    return result;
}

inline bool line(std::istream& input, std::string& result) {
    if (!std::getline(input, result)) return false;
    if (result.ends_with('\r')) result.pop_back();
    return true;
}

inline std::ifstream table(const Path& path, std::string_view header) {
    std::ifstream input(path, std::ios::binary);
    std::string row;
    if (!line(input, row) || row != header) {
        throw std::runtime_error("Missing or invalid TSV header: " + path.string());
    }
    return input;
}

struct Function {
    Word remaining_bytes{};
    std::string body;
};

struct Range {
    Word first, last, entry;
};

using Functions = std::map<Word, Function>;

struct Maps {
    Functions functions;
    std::vector<Range> ranges;

    explicit Maps(const Path& directory) {
        auto entries = table(directory / "functions.tsv", "entry\tname\tbody_bytes\tthunk");
        std::string row;
        while (line(entries, row)) {
            const auto values = fields<4>(row);
            const auto size = number(values[2]);
            if (!size || !functions.emplace(number(values[0], 16), Function{size, {}}).second) {
                throw std::runtime_error("Empty or duplicate function");
            }
        }
        auto bodies = table(directory / "function-ranges.tsv", "entry\tstart\tend_inclusive");
        while (line(bodies, row)) {
            const auto values = fields<3>(row);
            Range range{number(values[1], 16), number(values[2], 16), number(values[0], 16)};
            auto owner = functions.find(range.entry);
            if (owner == functions.end() || range.first > range.last ||
                std::uint64_t{range.last} - range.first + 1 > owner->second.remaining_bytes) {
                throw std::runtime_error("Invalid function range at " + hex(range.first));
            }
            owner->second.remaining_bytes -= range.last - range.first + 1;
            ranges.push_back(range);
        }
        if (!entries.eof() || !bodies.eof() || functions.empty()) {
            throw std::runtime_error("Incomplete function map");
        }
        std::ranges::sort(ranges, {}, &Range::first);
        for (std::size_t i = 1; i < ranges.size(); ++i) {
            if (ranges[i - 1].last >= ranges[i].first) {
                throw std::runtime_error("Overlapping function ranges at " + hex(ranges[i].first));
            }
        }
        for (const auto& [entry, body] : functions) {
            const auto* range = containing(entry);
            if (body.remaining_bytes || !range || range->entry != entry) {
                throw std::runtime_error("Function entry or body size differs at " + hex(entry));
            }
        }
    }

    const Range* containing(Word address) const {
        const auto after = std::ranges::upper_bound(ranges, address, {}, &Range::first);
        if (after == ranges.begin() || address > (after - 1)->last) return nullptr;
        return &*(after - 1);
    }
};

} // namespace analysis_map
