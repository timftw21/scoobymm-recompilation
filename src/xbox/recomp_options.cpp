#include "recomp_options.h"
#include "module_symbols.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace xbox_native {
namespace {
using Word = std::uint32_t;

// The engine's HUD interface (cdecl).
namespace hud {
constexpr Word create = 0x00, hide = 0x08, show = 0x0C, position = 0x18, scale = 0x1C, name = 0x28, text = 0x5C, font = 0x6C;
constexpr Word visibility = 0x100;
}
constexpr Word sound_bank = 0x2A000000, sound_navigate = 0xA, sound_confirm = 0xD, sound_back = 0xE;
// The return address of a host call into guest code: outside every module.
constexpr Word host_return = 0xFFFFFFD0;
// The new strings take the same ids in every text table, above the largest
// table on the disc (430 records).
constexpr Word first_text = 0x200;
// CAMERA's label box is .3 wide and wraps longer names. Keep its right edge.
constexpr float label_width = .42f;
// The flower sits left of a centered Options entry. The game's English
// positions work out to 0.394 less 0.008 per character.
float entry_selector_x(std::string_view text) { return .394f - .008f * float(text.size()); }

// ---- Settings ------------------------------------------------------------
struct Choice { std::string text; int value; };
struct Row {
    std::string label;
    std::vector<Choice> choices;
    int (*get)(const PlayerSettings&);
    void (*set)(PlayerSettings&, int);
};
std::vector<Choice> switch_choices() { return {{"off", 0}, {"on", 1}}; }
std::vector<Choice> multiples(std::initializer_list<int> values, const char* first) {
    std::vector<Choice> out;
    for (const int value : values) out.push_back({value == 1 ? first : std::to_string(value) + "x", value});
    return out;
}
// Each category opens its own page, as LANGUAGE and SOUND OPTIONS do.
struct Category {
    std::string name, title;
    std::vector<Row> rows;
};
std::vector<Category> categories() {
    std::vector<Choice> rates;
    for (const int rate : {30, 60, 75, 90, 120, 144, 165, 180, 240, 360, 480}) rates.push_back({std::to_string(rate), rate});
    rates.push_back({"unlimited", 0});
    std::vector<Choice> levels;
    for (int level = 0; level <= 100; level += 10) levels.push_back({std::to_string(level), level});
    return {
        {"display", "DISPLAY", {
            {"fullscreen", switch_choices(), [](const PlayerSettings& s) { return int(s.fullscreen); }, [](PlayerSettings& s, int v) { s.fullscreen = v != 0; }},
            {"widescreen", switch_choices(), [](const PlayerSettings& s) { return int(s.widescreen); }, [](PlayerSettings& s, int v) { s.widescreen = v != 0; }},
            {"vsync", switch_choices(), [](const PlayerSettings& s) { return int(s.vsync); }, [](PlayerSettings& s, int v) { s.vsync = v != 0; }},
            {"frame rate", rates, [](const PlayerSettings& s) { return s.refresh_rate; }, [](PlayerSettings& s, int v) { s.refresh_rate = v; }},
        }},
        {"enhancements", "ENHANCEMENTS", {
            {"internal resolution", multiples({1, 2, 3, 4, 5, 6}, "1x"),
                [](const PlayerSettings& s) { return s.enhancements.scale; }, [](PlayerSettings& s, int v) { s.enhancements.scale = v; }},
            {"antialiasing", multiples({1, 2, 4, 8}, "off"),
                [](const PlayerSettings& s) { return s.enhancements.samples; }, [](PlayerSettings& s, int v) { s.enhancements.samples = v; }},
            {"fxaa", switch_choices(), [](const PlayerSettings& s) { return int(s.enhancements.fxaa); },
                [](PlayerSettings& s, int v) { s.enhancements.fxaa = v != 0; }},
            {"texture filtering", multiples({1, 2, 4, 8, 16}, "normal"),
                [](const PlayerSettings& s) { return s.enhancements.anisotropy; }, [](PlayerSettings& s, int v) { s.enhancements.anisotropy = v; }},
        }},
        {"audio", "AUDIO", {
            {"master volume", levels, [](const PlayerSettings& s) { return s.volume; }, [](PlayerSettings& s, int v) { s.volume = v; }},
            {"mute", switch_choices(), [](const PlayerSettings& s) { return int(s.muted); }, [](PlayerSettings& s, int v) { s.muted = v != 0; }},
            {"mute when unfocused", switch_choices(), [](const PlayerSettings& s) { return int(s.mute_unfocused); },
                [](PlayerSettings& s, int v) { s.mute_unfocused = v != 0; }},
        }},
    };
}

// ---- Text ----------------------------------------------------------------
// Text holds glyph indices. Each font ends with the glyph of every ASCII
// character; 0 and 1 mark characters it lacks, and spaces are 0xFFFE.
using FontMap = std::array<std::uint16_t, 128>;
std::optional<FontMap> font_map(std::span<const std::byte> font) {
    if (font.size() < 0x400) return {};
    FontMap map;
    std::memcpy(map.data(), font.data() + font.size() - sizeof map, sizeof map);
    return map;
}
std::optional<std::vector<std::byte>> encode(const FontMap& font, std::string_view text) {
    std::vector<std::byte> out;
    for (const char ch : text) {
        const auto code = static_cast<unsigned char>(ch);
        std::uint16_t glyph = 0;
        if (ch == ' ') glyph = 0xFFFE;
        else if (code < font.size()) {
            glyph = font[code];
            // A font without the capital draws it as a small capital.
            if (glyph <= 1 && ch >= 'A' && ch <= 'Z') glyph = font[code - 'A' + 'a'];
        }
        if (glyph <= 1) return {};
        out.push_back(static_cast<std::byte>(glyph & 0xFF));
        out.push_back(static_cast<std::byte>(glyph >> 8));
    }
    return out;
}

// ---- Panels --------------------------------------------------------------
struct Spec {
    Word name, type, parent, flags;
    float scale_x, scale_y, x, y;
    Word layer, font, text;
    std::array<std::int8_t, 3> colors;
};
Word bits(float value) { return std::bit_cast<Word>(value); }
Word color(int value) { return static_cast<Word>(value); }
// CreateHudWins passes the colors a spec names, up to the first unset one.
std::vector<Word> spec_colors(const Spec& s) {
    std::vector<Word> out;
    for (const auto c : s.colors) {
        if (c == -1) break;
        out.push_back(color(c));
    }
    return out;
}
// MenuRefresh's colors for highlighted and plain entries.
constexpr std::array<Word, 2> lit{6, 6}, title_plain{1, 7}, pause_plain{7, 7};

enum class Kind { title, pause };
enum class Input { updown, sideways, confirm, back, start, stick, pause_open, pause_close };
// Window specs: the panel's root, the menu window, its first entry, the
// entries above and below Recomp Options, its last entry, and the Options
// page with its title, CAMERA and VIBRATION labels, an entry and CAMERA's value.
struct Specs { Word root, menu, first, anchor, next, last, options, options_title, camera, vibration, entry, value; };
// Text the specs name, checked before the menu changes.
struct Expected { Word spec, text; };

// Panel.c in LEVEL\INTRO\GEN\INTRO.XBR (title) and the per-level panel_base
// (pause). Offsets come from the objects' COFF symbols and code.
struct Layout {
    Kind kind{};
    Word specs{}, spec_size{}, selectors{};
    Word local_data{}, global_data{}, hud{}, sound{}, extra{}, game_obj{}, input{};
    Word show_main_choices{}, set_prompts{}, draw_new_menu{};
    // Local data fields.
    Word handles{}, selection{}, current{}, last{}, selector{}, input_timer{}, lock{};
    float stick{};
    std::map<Word, Input> callbacks;
};
constexpr Specs title_specs{0x10, 0x14, 0x15, 0x16, 0x17, 0x19, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x75};
constexpr Specs pause_specs{0x00, 0x07, 0x09, 0x09, 0x0A, 0x0D, 0x14, 0x15, 0x16, 0x17, 0x18, 0x1A};
constexpr std::array<Expected, 7> title_expected{{{0x15, 0x13B}, {0x16, 0x13C}, {0x17, 0x13D}, {0x18, 0x13E},
                                              {0x6E, 0x2F}, {0x6F, 0x32}, {0x71, 0x31}}};
constexpr std::array<Expected, 6> pause_expected{{{0x09, 0x25}, {0x0A, 0x27}, {0x15, 0x2F}, {0x16, 0x32}, {0x18, 0x34}, {0x1A, 0x39}}};
// The title's art and Options backdrops.
constexpr Word title_art = 0x11, backdrop = 0x12, backdrop2 = 0x13, controller_lost = 0x3BC;
constexpr Word pause_selector = 0x06;
// The pause panel's input sets and states, and its prompt window with the
// "back" text the pause menu shows.
constexpr Word pause_input = 8, options_input = 0xA, pause_state = 4, options_state = 5;
constexpr Word prompt_window = 0x02, prompt_back = 0x05, back_prompt = 8;

std::uint8_t code_byte(const CompiledModule& module, Word offset) {
    const auto nibble = [](char digit) { return digit <= '9' ? digit - '0' : digit - 'A' + 10; };
    return static_cast<std::uint8_t>(16 * nibble(module.code_hex[2 * offset]) + nibble(module.code_hex[2 * offset + 1]));
}
// The field displacements of instructions in a routine: the 32-bit operand
// after each occurrence of an opcode and ModR/M byte pair (or triple).
std::vector<Word> displacements(const CompiledModule& module, Word start, Word span, std::initializer_list<std::uint8_t> opcode) {
    std::vector<Word> out;
    const Word size = static_cast<Word>(module.code_hex.size() / 2);
    const Word length = static_cast<Word>(opcode.size());
    for (Word at = start; at + length + 4 <= std::min(size, start + span); ++at) {
        Word k = 0;
        for (const auto b : opcode) {
            if (code_byte(module, at + k) != b) break;
            ++k;
        }
        if (k != length) continue;
        Word value = 0;
        for (Word i = 0; i < 4; ++i) value |= Word(code_byte(module, at + length + i)) << (8 * i);
        out.push_back(value);
    }
    return out;
}

std::optional<Layout> resolve(const CompiledModule& module) {
    if (module.name != "panel") return {};
    const auto symbols = module_symbols(module);
    bool found = true;
    const auto at = [&](std::string_view name) {
        const auto offset = module_symbol(symbols, name);
        found &= offset.has_value();
        return offset.value_or(0);
    };
    Layout l;
    if (module_symbol(symbols, "_goTitleWindowsSpecsBase")) {
        l.kind = Kind::title;
        l.specs = at("_goTitleWindowsSpecsBase");
        l.spec_size = 0x70;
        l.selectors = at("_goPauseSelectorInfo");
        l.local_data = at("_pPanelLocalData");
        l.global_data = at("_pPanelGlobalData");
        l.extra = at("_pExtra");
        l.sound = at("_pSound");
        l.hud = at("_pHud");
        l.show_main_choices = at("_panel_baseFUNC_ShowMainChoices");
        l.set_prompts = at("_panel_baseFUNC_SetPrompts");
        l.callbacks = {{at("_PanelCB_INPUT_X"), Input::confirm}, {at("_PanelCB_INPUT_UPDOWN"), Input::updown},
                       {at("_PanelCB_INPUT_LR"), Input::sideways}, {at("_PanelCB_INPUT_TRIANGLE"), Input::back},
                       {at("_PanelCB_INPUT_STICK_NOTOG"), Input::stick}, {at("_PanelCB_INPUT_START"), Input::start},
                       {at("_PanelCB_INPUT_START2"), Input::start}, {at("_PanelCB_INPUT_START3"), Input::start},
                       {at("_PanelCB_INPUT_START4"), Input::start}};
        // _pPanelLocalData fields.
        l.handles = 0x40;
        l.selector = 0x37C;
        l.selection = 0x380;
        l.current = 0x384;
        l.input_timer = 0x3A8;
        l.stick = .33f;
    } else if (module_symbol(symbols, "_goPauseWindowsSpecsBase")) {
        l.kind = Kind::pause;
        l.specs = at("_goPauseWindowsSpecsBase");
        l.spec_size = 0x60;
        l.selectors = at("_goPauseSelectorInfo");
        l.local_data = at("_pLocalData_BasePanel");
        l.global_data = at("_pGlobalData_BasePanel");
        l.sound = at("_pSound");
        l.hud = at("_pHud");
        l.game_obj = at("_pGameObj");
        l.input = at("_pInput");
        l.draw_new_menu = at("_panel_baseFUNC_DrawNewMenu");
        // The pause menu's input set, then the Options input set the pages
        // use like Game Options.
        l.callbacks = {{at("_panel_baseCB_MENU_CONFIRM"), Input::confirm}, {at("_panel_baseCB_MENU_STICK_UPDOWN"), Input::updown},
                       {at("_panel_baseCB_MENU_STICK_LEFTRIGHT"), Input::sideways}, {at("_panel_baseCB_MENU_CANCEL"), Input::back},
                       {at("_panel_baseCB_PAUSE_MENU_OPEN"), Input::pause_open},
                       {at("_panel_baseCB_PAUSE_MENU_CLOSE"), Input::pause_close},
                       {at("_panel_baseCB_OPTIONS_CONFIRM"), Input::confirm}, {at("_panel_baseCB_OPTIONS_STICK_UPDOWN"), Input::updown},
                       {at("_panel_baseCB_OPTIONS_STICK_LEFTRIGHT"), Input::sideways}, {at("_panel_baseCB_OPTIONS_CANCEL"), Input::back}};
        if (!found) return {};
        // Six levels' local data has an extra field before the window handles.
        // DrawNewMenu names the fields: it compares the current window with
        // -1, loads its handle, then stores the selection, current window,
        // first and last entries.
        const auto compared = displacements(module, l.draw_new_menu, 0x100, {0x83, 0xB9});
        const auto handles = displacements(module, l.draw_new_menu, 0x100, {0x8B, 0x84, 0x08});
        const auto from_ebx = displacements(module, l.draw_new_menu, 0x100, {0x89, 0x9A});
        const auto from_eax = displacements(module, l.draw_new_menu, 0x100, {0x89, 0x82});
        if (compared.size() != 1 || handles.empty() || from_ebx.size() != 2 || from_eax.size() != 2 ||
            from_eax[0] != compared[0] || from_eax[1] != from_ebx[1] + 4 || compared[0] != from_ebx[1] + 8)
            return {};
        l.current = compared[0];
        l.handles = handles[0];
        l.selection = from_ebx[0];
        l.last = from_eax[1];
        l.selector = l.handles + 4 * pause_selector;
        l.lock = 0x18;
        l.stick = .3f;
    } else {
        return {};
    }
    if (!found) return {};
    return l;
}
}

struct RecompOptions::Impl {
    Host host;
    std::vector<Category> menu = categories();
    // Text ids: the menu entry, the list title, and for each category its
    // entry, page title and row labels, then the value names.
    struct Text {
        Word entry{}, title{};
        std::vector<Word> labels;
    };
    Word menu_text{}, title_text{};
    std::vector<Text> texts;
    std::map<std::string, Word> value_text;
    bool text_ready = false;
    std::map<const CompiledModule*, std::optional<Layout>> layouts;
    // The loaded panel and the widgets added to it: the menu entry, the
    // category list and a page for each category.
    struct Page {
        Word window{};
        std::vector<Word> labels, values;
    };
    const Layout* layout{};
    const Specs* specs{};
    Word base{}, local{};
    bool known = false, built = false, highlighted = false, open = false, inside = false;
    Word item{}, list{}, menu_window{}, menu_last{};
    std::vector<Word> entries;
    std::vector<Page> pages;
    std::size_t category = 0, row = 0;
    std::chrono::steady_clock::time_point settled{};
    NativeFunction original{};
    Cpu* cpu{};
    Memory* memory{};

    static Impl* active;
    explicit Impl(Host h) : host(h) {}

    // ---- Guest calls -----------------------------------------------------
    Word call(Word address, std::span<const Word> args) {
        auto& c = *cpu;
        auto& m = *memory;
        const auto registers = c.registers;
        const auto flags = c.flags;
        const auto pc = c.eip;
        const Word stack = registers[esp] - 0x200 - 4 * static_cast<Word>(args.size());
        m.store<Word>(stack, host_return);
        Word slot = 0;
        for (const auto arg : args) m.store<Word>(stack + 4 + 4 * slot++, arg);
        c.registers[esp] = stack;
        c.eip = host_return;
        invoke_native(c, m, address);
        if (c.eip != host_return || c.registers[esp] != stack + 4)
            throw NativeBoundary(address, "A Recomp Options call into the game returned with an unexpected stack");
        const auto result = c.registers[eax];
        c.registers = registers;
        c.flags = flags;
        c.eip = pc;
        return result;
    }
    Word call(Word address, std::initializer_list<Word> args) { return call(address, std::span(args.begin(), args.size())); }
    Word load(Word address) const { return memory->load<Word>(address); }
    Word local_field(Word offset) const { return load(local + offset); }
    void set_local(Word offset, Word value) { memory->store<Word>(local + offset, value); }
    Word handle(Word index) const { return local_field(layout->handles + 4 * index); }
    Word method(Word cell, Word slot) const { return load(load(base + cell) + slot); }
    void hud_call(Word slot, std::initializer_list<Word> args) { call(method(layout->hud, slot), args); }
    void sound(Word id) { call(method(layout->sound, 0), {sound_bank, id, 0}); }
    void show(Word h, bool visible) { hud_call(visible ? hud::show : hud::hide, {h, hud::visibility}); }
    Spec spec(Word index) const {
        const Word at = base + layout->specs + index * layout->spec_size;
        const auto f = [&](Word offset) { return std::bit_cast<float>(load(at + offset)); };
        const auto byte = [&](Word offset) { return static_cast<std::int8_t>(memory->load<std::uint8_t>(at + offset)); };
        return {load(at), load(at + 4), load(at + 8), load(at + 12), f(0x10), f(0x14), f(0x18), f(0x1C),
                load(at + 0x20), load(at + 0x28), load(at + 0x2C), {byte(0x30), byte(0x31), byte(0x32)}};
    }
    // _goPauseSelectorInfo holds the flower's x and y for each entry in
    // each language. Recomp Options' text is English in all of them.
    Word selector_cell(Word index, Word language, Word axis) const { return base + layout->selectors + index * 24 + language * 8 + axis; }
    float selector_at(Word index, Word axis) const { return std::bit_cast<float>(load(selector_cell(index, 0, axis))); }
    float row_y(std::size_t row) const {
        const float top = spec(specs->camera).y;
        return top + (spec(specs->vibration).y - top) * float(row);
    }
    float selector_rise() const { return spec(specs->camera).y - selector_at(specs->camera, 4); }

    // Panel.c restarts the title's inactivity timer at each input callback.
    void touch() {
        if (layout->kind == Kind::title) call(method(layout->extra, 8), {local_field(layout->input_timer)});
    }
    // The title's button prompts. The pause panel instead moves between its
    // pause and Options states and input sets, as Game Options does
    // (MainMenuConfirm, CB_OPTIONS_CANCEL); the states set the prompts.
    void prompts(bool options) {
        if (layout->kind == Kind::title) return void(call(base + layout->set_prompts, {options ? 5u : 1u}));
        const Word from = options ? pause_input : options_input, to = options ? options_input : pause_input;
        if (!options) {
            show(handle(prompt_window), true);
            set_text(handle(prompt_back), back_prompt, {});
        }
        call(method(layout->input, 0x14), {from, 0});
        call(method(layout->input, 0x14), {to, 1});
        call(method(layout->input, 0x0C), {to, options ? 0x196u : 0x192u});
        call(method(layout->game_obj, 4), {sound_bank, options ? options_state : pause_state});
    }
    std::span<const Word> plain() const { return layout->kind == Kind::title ? std::span<const Word>(title_plain) : pause_plain; }

    Word create(const Spec& s, Word parent, float y, Word text, std::span<const Word> colors) {
        const Word h = call(method(layout->hud, hud::create), {s.type, parent, s.flags, s.layer});
        hud_call(hud::scale, {h, bits(s.scale_x), bits(s.scale_y)});
        hud_call(hud::position, {h, bits(s.x), bits(y)});
        hud_call(hud::name, {h, s.name});
        if (s.type == 1) {
            hud_call(hud::font, {h, s.font});
            set_text(h, text, colors);
        }
        return h;
    }
    void set_text(Word h, Word text, std::span<const Word> colors) {
        std::vector<Word> args{h, text};
        args.insert(args.end(), colors.begin(), colors.end());
        call(method(layout->hud, hud::text), args);
    }
    void highlight(Word h, Word text, bool on) { set_text(h, text, on ? std::span<const Word>(lit) : plain()); }
    void move_selector(float x, float y) { hud_call(hud::position, {local_field(layout->selector), bits(x), bits(y)}); }

    // ---- Layout ----------------------------------------------------------
    bool panel_ready() const {
        return local && handle(specs->menu) && handle(specs->root) && local_field(layout->selector) && handle(specs->anchor) &&
               handle(specs->next);
    }
    void build() {
        built = true;
        menu_window = handle(specs->menu);
        // The new entry takes the place of the entry below it, which moves
        // down with the entries after it. The flower's positions move too.
        const auto anchor = spec(specs->anchor), next = spec(specs->next);
        const float step = next.y - anchor.y;
        item = create(anchor, menu_window, next.y, menu_text, plain());
        for (Word index = specs->next; index <= specs->last; ++index) {
            if (const Word h = handle(index)) {
                const auto entry = spec(index);
                hud_call(hud::position, {h, bits(entry.x), bits(entry.y + step)});
            }
            for (Word language = 0; language < 3; ++language) {
                const Word y = selector_cell(index, language, 4);
                memory->store<Word>(y, bits(std::bit_cast<float>(load(y)) + step));
            }
        }
        // The pages copy the Options page: its window and title, an entry's
        // style for the category list, and the CAMERA row's label and value
        // styles for each category's settings.
        const auto window = spec(specs->options);
        const auto title_spec = spec(specs->options_title);
        const auto entry = spec(specs->entry);
        auto label = spec(specs->camera);
        const auto value = spec(specs->value);
        label.x -= label_width - label.scale_x;
        label.scale_x = label_width;
        const auto title_colors = spec_colors(title_spec);
        const auto page = [&](Word text) {
            const Word h = create(window, handle(specs->root), window.y, 0, {});
            show(h, false);
            create(title_spec, h, title_spec.y, text, title_colors);
            return h;
        };
        list = page(title_text);
        entries.clear();
        pages.clear();
        for (std::size_t i = 0; i < menu.size(); ++i) entries.push_back(create(entry, list, row_y(i), texts[i].entry, plain()));
        const auto value_colors = spec_colors(value);
        for (std::size_t i = 0; i < menu.size(); ++i) {
            Page p{page(texts[i].title), {}, {}};
            for (std::size_t j = 0; j < menu[i].rows.size(); ++j) {
                p.labels.push_back(create(label, p.window, row_y(j), texts[i].labels[j], plain()));
                p.values.push_back(create(value, p.window, row_y(j), value_name(menu[i].rows[j]), value_colors));
            }
            pages.push_back(std::move(p));
        }
    }
    const Choice& choice(const Row& r) const {
        const int current = r.get(host.settings);
        const auto found = std::ranges::find(r.choices, current, &Choice::value);
        if (found != r.choices.end()) return *found;
        // A setting outside the listed choices shows the nearest one.
        return *std::ranges::min_element(r.choices, {}, [&](const Choice& c) { return std::abs(c.value - current); });
    }
    Word value_name(const Row& r) const { return value_text.at(choice(r).text); }
    void refresh() {
        if (inside) {
            const auto& p = pages[category];
            const auto& rows = menu[category].rows;
            const auto value_colors = spec_colors(spec(specs->value));
            for (std::size_t j = 0; j < rows.size(); ++j) {
                highlight(p.labels[j], texts[category].labels[j], j == row);
                set_text(p.values[j], value_name(rows[j]), value_colors);
            }
            return move_selector(selector_at(specs->camera, 0), row_y(row) - selector_rise());
        }
        for (std::size_t i = 0; i < menu.size(); ++i) highlight(entries[i], texts[i].entry, i == category);
        move_selector(entry_selector_x(menu[category].name), row_y(category) - selector_rise());
    }
    void select_entry() {
        // The entry the panel had highlighted returns to its plain colors; the
        // panel's own selection rests on the entry above this one.
        const Word previous = local_field(layout->selection);
        if (previous >= specs->first && previous <= specs->last && handle(previous))
            highlight(handle(previous), spec(previous).text, false);
        highlighted = true;
        set_local(layout->selection, specs->anchor);
        highlight(item, menu_text, true);
        const float step = spec(specs->next).y - spec(specs->anchor).y;
        // The title's flower sits closer to the longer entry name.
        const float x = layout->kind == Kind::title ? .44f : selector_at(specs->anchor, 0);
        move_selector(x, selector_at(specs->anchor, 4) + step);
    }
    void unselect_entry() {
        highlighted = false;
        highlight(item, menu_text, false);
        set_local(layout->selection, specs->anchor);
    }
    // While a page is open the panel's current window is its Options page,
    // so its own input leaves the menu alone.
    Word marker() const { return specs->options; }
    void open_page() {
        sound(sound_confirm);
        if (layout->kind == Kind::title) {
            show(handle(title_art), false);
            show(handle(backdrop), true);
            show(handle(backdrop2), true);
            show(handle(specs->menu), false);
            set_local(layout->current, marker());
        } else {
            // The game's own change to Game Options: it hides the pause menu
            // and its side panels. Its window stays hidden behind ours.
            menu_last = local_field(layout->last);
            call(base + layout->draw_new_menu, {specs->options, specs->camera, specs->camera + 3});
            show(handle(specs->options), false);
        }
        highlighted = false;
        show(list, true);
        open = true;
        inside = false;
        category = 0;
        refresh();
        prompts(true);
        settled = std::chrono::steady_clock::now();
    }
    void enter_category() {
        sound(sound_confirm);
        show(list, false);
        show(pages[category].window, true);
        inside = true;
        row = 0;
        refresh();
        settled = std::chrono::steady_clock::now();
    }
    void leave_category() {
        sound(sound_back);
        show(pages[category].window, false);
        show(list, true);
        inside = false;
        refresh();
        settled = std::chrono::steady_clock::now();
    }
    void hide_pages() {
        show(list, false);
        for (const auto& p : pages) show(p.window, false);
        open = inside = false;
    }
    void close_page() {
        sound(sound_back);
        hide_pages();
        if (layout->kind == Kind::title) {
            prompts(false);
            call(base + layout->show_main_choices, {});
            show(handle(backdrop), false);
            show(handle(backdrop2), false);
            show(handle(title_art), true);
        } else {
            call(base + layout->draw_new_menu, {specs->menu, specs->anchor, menu_last});
            prompts(false);
        }
        select_entry();
        settled = std::chrono::steady_clock::now();
    }
    void change(int step) {
        const auto& r = menu[category].rows[row];
        const auto& now = choice(r);
        auto at = std::ranges::find(r.choices, now.value, &Choice::value) - r.choices.begin();
        const int limit = r.label == "antialiasing" ? host.max_samples() : INT32_MAX;
        for (std::size_t tries = 0; tries < r.choices.size(); ++tries) {
            at = (at + step + std::ptrdiff_t(r.choices.size())) % std::ptrdiff_t(r.choices.size());
            if (r.choices[at].value <= limit) break;
        }
        r.set(host.settings, r.choices[at].value);
        host.apply();
        sound(sound_navigate);
        refresh();
    }
    bool settling() const { return std::chrono::steady_clock::now() - settled < std::chrono::milliseconds(250); }
    bool on_menu() const { return local_field(layout->current) == specs->menu; }
    // A message box or a missing controller owns the input.
    bool modal() const {
        if (layout->kind == Kind::title) return load(load(base + layout->global_data) + controller_lost) != 0;
        return local_field(layout->lock) != 0;
    }

    // ---- Callbacks -------------------------------------------------------
    void finish() {
        auto& c = *cpu;
        c.registers[eax] = 0;
        c.eip = load(c.registers[esp]);
        c.registers[esp] += 4;
    }
    void run_original() {
        original(*cpu, *memory);
        // The menu changes inside the original callbacks. Its widgets exist
        // once the panel has created its own.
        if (!built && panel_ready()) build();
        if (highlighted && (!on_menu() || local_field(layout->selection) != specs->anchor)) {
            highlighted = false;
            highlight(item, menu_text, false);
        }
    }
    float stick_axis(Word offset) const { return std::bit_cast<float>(load(load(cpu->registers[esp] + 4) + offset)); }
    void updown() {
        const float y = stick_axis(4);
        const int step = y < -layout->stick ? 1 : y > layout->stick ? -1 : 0;
        if (open) {
            touch();
            auto& at = inside ? row : category;
            const std::size_t count = inside ? menu[category].rows.size() : menu.size();
            if (step && count) {
                at = (at + count + step) % count;
                sound(sound_navigate);
                refresh();
            }
            return finish();
        }
        if (!built || !on_menu() || modal()) return run_original();
        const Word selected = local_field(layout->selection);
        if (highlighted) {
            if (!step) return finish();
            highlighted = false;
            highlight(item, menu_text, false);
            // Leave as the original would from the neighbouring entry.
            set_local(layout->selection, step > 0 ? specs->anchor : specs->next);
            return run_original();
        }
        if ((step > 0 && selected == specs->anchor) || (step < 0 && selected == specs->next)) {
            touch();
            sound(sound_navigate);
            select_entry();
            return finish();
        }
        run_original();
    }
    void confirm() {
        if (open) {
            touch();
            if (settling()) return finish();
            if (inside) change(1);
            else enter_category();
            return finish();
        }
        if (highlighted && on_menu() && !modal()) {
            touch();
            open_page();
            return finish();
        }
        run_original();
    }
    void sideways() {
        if (!open) return run_original();
        touch();
        const float x = stick_axis(0);
        if (inside && x < -layout->stick) change(-1);
        else if (inside && x > layout->stick) change(1);
        finish();
    }
    void back() {
        if (!open) {
            if (highlighted) unselect_entry();
            return run_original();
        }
        touch();
        if (settling()) return finish();
        if (inside) leave_category();
        else close_page();
        finish();
    }
    // START picks the title menu's entry; it closes the pause menu.
    void start() {
        if (open) {
            touch();
            return finish();
        }
        if (highlighted && on_menu() && !modal()) return confirm();
        run_original();
    }
    void pause_close() {
        if (open) return finish();
        if (highlighted) unselect_entry();
        run_original();
    }
    // The pause menu opens on Continue.
    void pause_open() {
        run_original();
        if (highlighted) {
            highlighted = false;
            highlight(item, menu_text, false);
        }
    }
    void stick() {
        if (!open) return run_original();
        touch();
        finish();
    }
    void dispatch(Input input) {
        switch (input) {
        case Input::updown: return updown();
        case Input::sideways: return sideways();
        case Input::confirm: return confirm();
        case Input::back: return back();
        case Input::start: return start();
        case Input::stick: return stick();
        case Input::pause_open: return pause_open();
        case Input::pause_close: return pause_close();
        }
    }
    bool expected_text() const {
        const auto check = [&](std::span<const Expected> texts) {
            return std::ranges::all_of(texts, [&](const Expected& t) { return spec(t.spec).text == t.text; });
        };
        return layout->kind == Kind::title ? check(title_expected) : check(pause_expected);
    }
    template <Input In> static void entry(Cpu& c, Memory& m) {
        auto& self = *active;
        self.cpu = &c;
        self.memory = &m;
        const Word local = m.load<Word>(self.base + self.layout->local_data);
        if (local != self.local) {
            // A new panel instance starts with the original menu.
            self.local = local;
            self.known = local && self.expected_text();
            self.built = self.highlighted = self.open = self.inside = false;
        }
        if (!self.known || !self.text_ready) return self.original(c, m);
        if (self.built && self.handle(self.specs->menu) != self.menu_window)
            self.built = self.highlighted = self.open = self.inside = false;
        // Something else left the pages' window: they close with it.
        if (self.open && self.local_field(self.layout->current) != self.marker()) self.hide_pages();
        self.dispatch(In);
    }
    static NativeFunction handler(Input input) {
        switch (input) {
        case Input::updown: return &entry<Input::updown>;
        case Input::sideways: return &entry<Input::sideways>;
        case Input::confirm: return &entry<Input::confirm>;
        case Input::back: return &entry<Input::back>;
        case Input::start: return &entry<Input::start>;
        case Input::stick: return &entry<Input::stick>;
        case Input::pause_open: return &entry<Input::pause_open>;
        case Input::pause_close: return &entry<Input::pause_close>;
        }
        return nullptr;
    }
};
RecompOptions::Impl* RecompOptions::Impl::active = nullptr;

RecompOptions::RecompOptions(Host host) : impl_(std::make_unique<Impl>(host)) {}
RecompOptions::~RecompOptions() {
    if (Impl::active == impl_.get()) Impl::active = nullptr;
}

bool RecompOptions::extends(std::string_view path) const {
    std::string upper(path);
    for (auto& ch : upper) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    return upper.starts_with("LEVEL\\") && (upper.ends_with("\\EN\\TEXT.TX") || upper.ends_with("\\FR\\TEXT.TX") ||
                                            upper.ends_with("\\DE\\TEXT.TX"));
}

std::string RecompOptions::font_path(std::string_view text_path) {
    return std::string(text_path.substr(0, text_path.rfind('\\') + 1)) + "font0.ft";
}

std::optional<std::vector<std::byte>> RecompOptions::text_table(std::span<const std::byte> file, std::span<const std::byte> font,
                                                                std::string_view path) {
    auto& s = *impl_;
    const auto glyphs = font_map(font);
    // A single-page table: four header words, the entry count, the offset
    // array with its byte length, then the text bytes with their size, an
    // encoding byte and their byte length (0x2EA60).
    const auto u32 = [&](std::size_t at) { Word v; std::memcpy(&v, file.data() + at, 4); return v; };
    if (!glyphs || file.size() < 33 || u32(12) != 1) return {};
    const Word count = u32(16), offset_bytes = u32(20);
    if (count > first_text || offset_bytes != 4 * count || 24ULL + offset_bytes + 9 > file.size()) return {};
    const std::size_t sizes = 24 + offset_bytes, data = sizes + 9;
    const Word size = u32(sizes), length = u32(sizes + 5);
    if (size != length || length % 4 || data + length != file.size()) return {};
    const auto record = [&](Word id) {
        const Word start = u32(24 + 4 * id), end = id + 1 < count ? u32(28 + 4 * id) : length;
        return file.subspan(data + start, end - start);
    };
    // A record ends with its glyphs, a zero glyph, a 0x8888 word when needed
    // to keep the next record four-byte aligned, and the words 1, 0. Each new
    // string copies a record of the same kind and replaces its glyphs.
    constexpr std::array<std::byte, 2> pad{std::byte{0x88}, std::byte{0x88}};
    constexpr std::array<std::byte, 2> terminator{};
    constexpr std::array<std::byte, 4> end{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    const auto clone = [&](Word id, std::string_view text) -> std::optional<std::vector<std::byte>> {
        if (id >= count) return {};
        const auto bytes = record(id);
        if (bytes.size() < 10 || !std::equal(end.begin(), end.end(), bytes.end() - 4)) return {};
        auto stop = bytes.size() - 4;
        if (std::equal(pad.begin(), pad.end(), bytes.begin() + stop - 2)) stop -= 2;
        if (!std::equal(terminator.begin(), terminator.end(), bytes.begin() + stop - 2)) return {};
        stop -= 2;
        auto start = stop;
        const auto word = [&](std::size_t at) { return std::to_integer<int>(bytes[at]) | std::to_integer<int>(bytes[at + 1]) << 8; };
        while (start >= 2 && word(start - 2) != 0 && word(start - 2) != 0x8888) start -= 2;
        const auto replaced = encode(*glyphs, text);
        if (start == stop || !replaced) return {};
        std::vector<std::byte> out(bytes.begin(), bytes.begin() + start);
        out.insert(out.end(), replaced->begin(), replaced->end());
        out.insert(out.end(), terminator.begin(), terminator.end());
        if (out.size() % 4) out.insert(out.end(), pad.begin(), pad.end());
        out.insert(out.end(), end.begin(), end.end());
        return out;
    };
    std::vector<std::vector<std::byte>> added;
    bool cloned = true;
    const auto add = [&](std::optional<std::vector<std::byte>> bytes) {
        if (!bytes) cloned = false;
        else added.push_back(std::move(*bytes));
        return first_text + Word(added.size()) - 1;
    };
    // Templates: the title menu's "options" (0x13D) or the pause menu's
    // "game options" (0x27), the Options page title (0x2F), its "sound
    // options" entry (0x34), "camera" label (0x32) and "normal" value (0x3D).
    std::string upper(path);
    for (auto& ch : upper) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    const Word menu_template = upper.starts_with("LEVEL\\INTRO\\") ? 0x13D : 0x27;
    const Word menu_text = add(clone(menu_template, "recomp options"));
    const Word title_text = add(clone(0x2F, "RECOMP OPTIONS"));
    std::vector<Impl::Text> texts(s.menu.size());
    for (std::size_t i = 0; i < s.menu.size(); ++i) {
        texts[i].entry = add(clone(0x34, s.menu[i].name));
        texts[i].title = add(clone(0x2F, s.menu[i].title));
        for (const auto& r : s.menu[i].rows) texts[i].labels.push_back(add(clone(0x32, r.label)));
    }
    std::map<std::string, Word> value_text;
    for (const auto& category : s.menu) for (const auto& r : category.rows) for (const auto& c : r.choices)
        if (!value_text.contains(c.text)) value_text[c.text] = add(clone(0x3D, c.text));
    // Every table carries the strings at the same ids, so the panel's text
    // follows whichever table the HUD reads. A table that cannot carry them
    // keeps the original menus.
    s.text_ready = cloned;
    if (!cloned) return {};
    s.menu_text = menu_text;
    s.title_text = title_text;
    s.texts = std::move(texts);
    s.value_text = std::move(value_text);

    // The ids between the table's own and the new strings repeat its first record.
    const Word total = first_text + Word(added.size());
    std::size_t extra = 0;
    for (const auto& a : added) extra += a.size();
    std::vector<std::byte> out;
    out.reserve(file.size() + 4 * (total - count) + extra);
    const auto put = [&](Word v) { const auto at = out.size(); out.resize(at + 4); std::memcpy(out.data() + at, &v, 4); };
    out.insert(out.end(), file.begin(), file.begin() + 16);
    put(total);
    put(4 * total);
    out.insert(out.end(), file.begin() + 24, file.begin() + 24 + offset_bytes);
    for (Word id = count; id < first_text; ++id) put(u32(24));
    Word at = length;
    for (const auto& a : added) { put(at); at += Word(a.size()); }
    put(at);
    out.push_back(file[sizes + 4]);
    put(at);
    out.insert(out.end(), file.begin() + data, file.end());
    for (const auto& a : added) out.insert(out.end(), a.begin(), a.end());
    return out;
}

NativeFunction RecompOptions::intercept(const CompiledModule& module, std::uint32_t base, std::uint32_t offset, NativeFunction original) {
    auto& s = *impl_;
    if (!s.text_ready || module.name != "panel") return nullptr;
    auto found = s.layouts.find(&module);
    if (found == s.layouts.end()) found = s.layouts.emplace(&module, resolve(module)).first;
    if (!found->second) return nullptr;
    const auto& layout = *found->second;
    const auto input = layout.callbacks.find(offset);
    if (input == layout.callbacks.end()) return nullptr;
    if (s.base != base || s.layout != &layout) {
        s.base = base;
        s.layout = &layout;
        s.specs = layout.kind == Kind::title ? &title_specs : &pause_specs;
        s.local = 0;
    }
    Impl::active = &s;
    s.original = original;
    return Impl::handler(input->second);
}
}
