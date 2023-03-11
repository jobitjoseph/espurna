/*

SETTINGS MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#include "espurna.h"

#include "crash.h"
#include "terminal.h"
#include "storage_eeprom.h"

#include <algorithm>
#include <vector>
#include <cstdlib>

#include <ArduinoJson.h>

// -----------------------------------------------------------------------------

namespace espurna {
namespace settings {
namespace {

// Depending on features enabled, we may end up with different left boundary
// Settings are written right-to-left, so we only have issues when there are a lot of key-values
// XXX: slightly hacky, because we EEPROMr.length() is 0 before we enter setup() code
static kvs_type kv_store(
    EepromStorage{},
#if DEBUG_SUPPORT
    EepromReservedSize + crashReservedSize(),
#else
    EepromReservedSize,
#endif
    EepromSize
);

} // namespace

namespace query {

const Setting* Setting::findFrom(const Setting* begin, const Setting* end, StringView key) {
    for (auto it = begin; it != end; ++it) {
        if ((*it) == key) {
            return it;
        }
    }

    return end;
}

String Setting::findValueFrom(const Setting* begin, const Setting* end, StringView key) {
    String out;

    const auto value = findFrom(begin, end, key);
    if (value != end) {
        out = (*value).value();
    }

    return out;
}

bool IndexedSetting::findSamePrefix(const IndexedSetting* begin, const IndexedSetting* end, StringView key) {
    for (auto it = begin; it != end; ++it) {
        if (samePrefix(key, (*it).prefix())) {
            return true;
        }
    }

    return false;
}

String IndexedSetting::findValueFrom(Iota iota, const IndexedSetting* begin, const IndexedSetting* end, StringView key) {
    String out;

    while (iota) {
        for (auto it = begin; it != end; ++it) {
            const auto expected = Key(
                (*it).prefix().toString(), *iota);
            if (key == expected.value()) {
                out = (*it).value(*iota);
                goto output;
            }
        }
        ++iota;
    }

output:
    return out;
}

namespace internal {
namespace {

std::forward_list<Handler> handlers;

} // namespace
} // namespace internal

String find(StringView key) {
    String out;

    for (const auto& handler : internal::handlers) {
        if (handler.check(key)) {
            out = handler.get(key);
            break;
        }
    }

    return out;
}

} // namespace query

namespace options {

bool EnumerationNumericHelper::check(const String& value) {
    if (value.length()) {
        if ((value.length() > 1) && (*value.begin() == '0')) {
            return false;
        }

        for (auto it = value.begin(); it != value.end(); ++it) {
            switch (*it) {
            case '0'...'9':
                break;
            default:
                return false;
            }
        }

        return true;
    }

    return false;
}

} // namespace options

ValueResult get(const String& key) {
    return kv_store.get(key);
}

bool set(const String& key, const String& value) {
    return kv_store.set(key, value);
}

bool del(const String& key) {
    return kv_store.del(key);
}

bool has(const String& key) {
    return kv_store.has(key);
}

Keys keys() {
    Keys out;
    kv_store.foreach([&](kvs_type::KeyValueResult&& kv) {
        out.push_back(kv.key.read());
    });

    return out;
}

size_t available() {
    return kv_store.available();
}

size_t size() {
    return kv_store.size();
}

void foreach(KeyValueResultCallback&& callback) {
    kv_store.foreach(callback);
}

void foreach_prefix(PrefixResultCallback&& callback, query::StringViewIterator prefixes) {
    kv_store.foreach([&](kvs_type::KeyValueResult&& kv) {
        auto key = kv.key.read();
        for (auto it = prefixes.begin(); it != prefixes.end(); ++it) {
            if (query::samePrefix(StringView{key}, (*it))) {
                callback((*it), std::move(key), kv.value);
            }
        }
    });
}

// --------------------------------------------------------------------------

namespace internal {
namespace duration_convert {
namespace {

// Input is always normalized to Pair, specific units are converted on demand

constexpr auto MicrosecondsPerSecond =
    duration::Microseconds{ duration::Microseconds::period::den };

void adjust_microseconds(Pair& pair) {
    if (pair.microseconds >= MicrosecondsPerSecond) {
        pair.seconds += duration::Seconds{ 1 };
        pair.microseconds -= MicrosecondsPerSecond;
    }
}

Pair from_chrono_duration(duration::Microseconds microseconds) {
    Pair out{};

    while (microseconds > MicrosecondsPerSecond) {
        out.seconds += duration::Seconds{ 1 };
        microseconds -= MicrosecondsPerSecond;
    }

    out.microseconds += microseconds;
    adjust_microseconds(out);

    return out;
}

constexpr auto MillisecondsPerSecond =
    duration::Milliseconds{ duration::Milliseconds::period::den };

Pair from_chrono_duration(duration::Milliseconds milliseconds) {
    Pair out{};

    while (milliseconds >= MillisecondsPerSecond) {
        out.seconds += duration::Seconds{ 1 };
        milliseconds -= MillisecondsPerSecond;
    }

    const auto microseconds =
        std::chrono::duration_cast<duration::Microseconds>(milliseconds);
    out.microseconds += microseconds;
    adjust_microseconds(out);

    return out;
}

Pair& operator+=(Pair& lhs, const Pair& rhs) {
    lhs.seconds += rhs.seconds;
    lhs.microseconds += rhs.microseconds;

    adjust_microseconds(lhs);

    return lhs;
}

template <typename T>
Pair& operator+=(Pair&, T);

template <>
Pair& operator+=(Pair& result, duration::Microseconds microseconds) {
    result += from_chrono_duration(microseconds);
    return result;
}

template <>
Pair& operator+=(Pair& result, duration::Milliseconds milliseconds) {
    result += from_chrono_duration(milliseconds);
    return result;
}

template <>
Pair& operator+=(Pair& result, duration::Hours hours) {
    result.seconds += std::chrono::duration_cast<duration::Seconds>(hours);
    return result;
}

template <>
Pair& operator+=(Pair& result, duration::Minutes minutes) {
    result.seconds += std::chrono::duration_cast<duration::Seconds>(minutes);
    return result;
}

template <>
Pair& operator+=(Pair& result, duration::Seconds seconds) {
    result.seconds += seconds;
    return result;
}

// Besides decimal or raw input with the specified ratio,
// string parser also supports type specifiers at the end of decimal number

enum class Type {
    Unknown,
    Seconds,
    Minutes,
    Hours,
};

bool validNextType(Type lhs, Type rhs) {
    switch (lhs) {
    case Type::Unknown:
        return true;
    case Type::Hours:
        return (rhs == Type::Minutes) || (rhs == Type::Seconds);
    case Type::Minutes:
        return (rhs == Type::Seconds);
    case Type::Seconds:
        break;
    }

    return false;
}

} // namespace

Result parse(StringView view, int num, int den) {
    Result out;
    out.ok = false;

    String token;
    Type last { Type::Unknown };
    Type type { Type::Unknown };

    const char* ptr { view.begin() };
    if (!view.begin() || !view.length()) {
        goto output;
    }

loop:
    while (ptr != view.end()) {
        switch (*ptr) {
        case '0'...'9':
            token += (*ptr);
            ++ptr;
            break;

        case 'h':
            if (validNextType(last, Type::Hours)) {
                type = Type::Hours;
                goto update_spec;
            }
            goto reset;

        case 'm':
            if (validNextType(last, Type::Minutes)) {
                type = Type::Minutes;
                goto update_spec;
            }
            goto reset;

        case 's':
            if (validNextType(last, Type::Seconds)) {
                type = Type::Seconds;
                goto update_spec;
            }
            goto reset;

        case 'e':
        case 'E':
            goto read_floating_exponent;

        case ',':
        case '.':
            if (out.ok) {
                goto reset;
            }

            goto read_floating;
        }
    }

    if (token.length()) {
        goto update_decimal;
    }

    goto output;

update_floating:
    {
        // only seconds and up, anything down of milli does not make sense here 
        if (den > 1) {
            goto reset;
        }

        char* endp { nullptr };
        auto value = strtod(token.c_str(), &endp);
        if (endp && (endp != token.c_str()) && endp[0] == '\0') {
            using Seconds = std::chrono::duration<float, std::ratio<1> >;

            const auto seconds = Seconds(num * value);
            const auto milliseconds =
                std::chrono::duration_cast<duration::Milliseconds>(seconds);

            out.value += milliseconds;
            out.ok = true;

            goto output;
        }

        goto reset;
    }

update_decimal:
    {
        const auto result = parseUnsigned(token, 10);
        if (result.ok) {
            // num and den are constexpr and bound to ratio types, so duration cast has to happen manually
            if ((num == 1) && (den == 1)) {
                out.value += duration::Seconds{ result.value };
            } else if ((num == 1) && (den > 1)) {
                out.value += duration::Seconds{ result.value / den };
                out.value += duration::Microseconds{ result.value % den * duration::Microseconds::period::den / den };
            } else if ((num > 1) && (den == 1)) {
                out.value += duration::Seconds{ result.value * num };
            } else {
                goto reset;
            }

            out.ok = true;
            goto output;
        }

        goto reset;
    }

update_spec:
    last = type;
    ++ptr;

    if (type != Type::Unknown) {
        const auto result = parseUnsigned(token, 10);
        if (result.ok) {
            switch (type) {
            case Type::Hours:
                out.value += duration::Hours { result.value };
                break;

            case Type::Minutes:
                out.value += duration::Minutes { result.value };
                break;

            case Type::Seconds:
                out.value += duration::Seconds { result.value };
                break;

            case Type::Unknown:
                goto reset;
            }

            out.ok = true;
            type = Type::Unknown;
            token = "";

            goto loop;
        }
    }

    goto reset;

read_floating:
    switch (*ptr) {
    case ',':
    case '.':
        token += '.';
        ++ptr;
        break;

    default:
        goto reset;
    }

    while (ptr != view.end()) {
        switch (*ptr) {
        case '0'...'9':
            token += (*ptr);
            break;

        case 'e':
        case 'E':
            goto read_floating_exponent;

        case ',':
        case '.':
            goto reset;
        }

        ++ptr;
    }

    goto update_floating;

read_floating_exponent:
    {
        token += (*ptr);
        ++ptr;

        bool sign { false };

        while (ptr != view.end()) {
            switch (*ptr) {
            case '-':
            case '+':
                if (sign) {
                    goto reset;
                }

                sign = true;

                token += (*ptr);
                ++ptr;
                break;

            case '0'...'9':
                token += (*ptr);
                ++ptr;
                break;

            default:
                goto reset;
            }
        }

        goto update_floating;
    }

reset:
    out.ok = false;

output:
    return out;
}

template <typename T>
T unchecked_parse(StringView view) {
    const auto result = duration_convert::parse(view, typename T::period{});

    if (result.ok) {
        return duration_convert::to_chrono_duration<T>(result.value);
    }

    return T{}.min();
}

} // namespace duration_convert

template <>
duration::Microseconds convert(const String& value) {
    return duration_convert::unchecked_parse<duration::Microseconds>(value);
}

template <>
duration::Milliseconds convert(const String& value) {
    return duration_convert::unchecked_parse<duration::Milliseconds>(value);
}

template <>
duration::Seconds convert(const String& value) {
    return duration_convert::unchecked_parse<duration::Seconds>(value);
}

template <>
std::chrono::duration<float> convert(const String& value) {
    return duration_convert::unchecked_parse<std::chrono::duration<float>>(value);
}

template <>
float convert(const String& value) {
    return strtod(value.c_str(), nullptr);
}

template <>
double convert(const String& value) {
    return strtod(value.c_str(), nullptr);
}

template <>
signed char convert(const String& value) {
    return value.toInt();
}

template <>
short convert(const String& value) {
    return value.toInt();
}

template <>
int convert(const String& value) {
    return value.toInt();
}

template <>
long convert(const String& value) {
    return value.toInt();
}

template <>
bool convert(const String& value) {
    if (value.length()) {
        if ((value == "0")
            || (value == "n")
            || (value == "no")
            || (value == "false")
            || (value == "off")) {
            return false;
        }

        return (value == "1")
            || (value == "y")
            || (value == "yes")
            || (value == "true")
            || (value == "on");
    }

    return false;
}

template <>
uint32_t convert(const String& value) {
    return parseUnsigned(value).value;
}

String serialize(uint32_t value, int base) {
    return formatUnsigned(value, base);
}

template <>
unsigned long convert(const String& value) {
    return convert<unsigned int>(value);
}

template <>
unsigned short convert(const String& value) {
    return convert<unsigned long>(value);
}

template <>
unsigned char convert(const String& value) {
    return convert<unsigned long>(value);
}

} // namespace internal

// TODO: UI needs this to avoid showing keys in storage order
std::vector<String> sorted_keys() {
    auto values = keys();
    std::sort(values.begin(), values.end(),
        [](const String& lhs, const String& rhs) -> bool {
            return rhs.compareTo(lhs) > 0;
        });

    return values;
}

#if TERMINAL_SUPPORT
namespace terminal {
namespace {

void dump(const ::terminal::CommandContext& ctx, const query::Setting* begin, const query::Setting* end) {
    for (auto it = begin; it != end; ++it) {
        ctx.output.printf_P(PSTR("> %s => %s\n"),
            (*it).key().c_str(), (*it).value().c_str());
    }
}

void dump(const ::terminal::CommandContext& ctx, const query::IndexedSetting* begin, const query::IndexedSetting* end, size_t index) {
    for (auto it = begin; it != end; ++it) {
        ctx.output.printf_P(PSTR("> %s%u => %s\n"),
            (*it).prefix().c_str(), index,
            (*it).value(index).c_str());
    }
}

namespace commands {

PROGMEM_STRING(Config, "CONFIG");

void config(::terminal::CommandContext&& ctx) {
    DynamicJsonBuffer jsonBuffer(1024);
    JsonObject& root = jsonBuffer.createObject();
    settingsGetJson(root);
    root.prettyPrintTo(ctx.output);
    terminalOK(ctx);
}

PROGMEM_STRING(Keys, "KEYS");

void keys(::terminal::CommandContext&& ctx) {
    const auto keys = settings::sorted_keys();

    String value;
    for (const auto& key : keys) {
        value = getSetting(key);
        ctx.output.printf_P(PSTR("> %s => \"%s\"\n"),
            key.c_str(), value.c_str());
    }

    const auto size = settings::size();
    if (size > 0) {
        const auto available = settings::available();
        ctx.output.printf_P(PSTR("Number of keys: %u\n"), keys.size());
        ctx.output.printf_P(PSTR("Available: %u bytes (%u%%)\n"),
                available, (100 * available) / size);
    }

    terminalOK(ctx);
}

PROGMEM_STRING(Gc, "GC");

void gc(::terminal::CommandContext&& ctx) {
    struct KeyRef {
        String key;
        size_t length;
    };

    using KeyRefs = std::vector<KeyRef>;
    KeyRefs refs;

    kv_store.foreach([&](kvs_type::KeyValueResult&& result) {
        refs.push_back(
            KeyRef{
                .key = result.key.read(),
                .length = result.key.length(),
            });
    });

    auto is_ascii = [](const String& value) -> bool {
        for (const auto& c : value) {
            if (!isascii(c)) {
                return false;
            }
        }

        return true;
    };

    std::vector<const String*> broken;
    for (const auto& ref : refs) {
        if ((ref.length != ref.key.length()) || !is_ascii(ref.key)) {
            broken.push_back(&ref.key);
        }
    }

    size_t count = 0;
    for (const auto& key : broken) {
        settings::del(*key);
        ++count;
    }

    ctx.output.printf_P("deleted %zu keys\n", count);
    terminalOK(ctx);
}

PROGMEM_STRING(Del, "DEL");

void del(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() < 2) {
        terminalError(ctx, F("del <key> [<key>...]"));
        return;
    }

    int result = 0;
    for (auto it = (ctx.argv.begin() + 1); it != ctx.argv.end(); ++it) {
        result += settings::del(*it);
    }

    if (result) {
        terminalOK(ctx);
    } else {
        terminalError(ctx, F("no keys were removed"));
    }
}

PROGMEM_STRING(Set, "SET");

void set(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() != 3) {
        terminalError(ctx, F("set <key> <value>"));
        return;
    }

    if (settings::set(ctx.argv[1], ctx.argv[2])) {
        terminalOK(ctx);
        return;
    }

    terminalError(ctx, F("could not set the key"));
}

PROGMEM_STRING(Get, "GET");

void get(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() < 2) {
        terminalError(ctx, F("get <key> [<key>...]"));
        return;
    }

    for (auto it = (ctx.argv.cbegin() + 1); it != ctx.argv.cend(); ++it) {
        auto result = settings::get(*it);
        if (!result) {
            const auto maybeValue = query::find(*it);
            if (maybeValue.length()) {
                ctx.output.printf_P(PSTR("> %s => %s (default)\n"),
                    (*it).c_str(), maybeValue.c_str());
            } else {
                ctx.output.printf_P(PSTR("> %s =>\n"), (*it).c_str());
            }
            continue;
        }

        ctx.output.printf_P(PSTR("> %s => \"%s\"\n"), (*it).c_str(), result.c_str());
    }

    terminalOK(ctx);
}

PROGMEM_STRING(Reload, "RELOAD");

void reload(::terminal::CommandContext&& ctx) {
    espurnaReload();
    terminalOK(ctx);
}

PROGMEM_STRING(FactoryReset, "FACTORY.RESET");

void factory_reset(::terminal::CommandContext&& ctx) {
    factoryReset();
    terminalOK(ctx);
}

[[gnu::unused]]
PROGMEM_STRING(Save, "SAVE");

[[gnu::unused]]
void save(::terminal::CommandContext&& ctx) {
    eepromCommit();
    terminalOK(ctx);
}

static constexpr ::terminal::Command List[] PROGMEM {
    {Config, commands::config},
    {Keys, commands::keys},
    {Gc, commands::gc},

    {Del, commands::del},
    {Set, commands::set},
    {Get, commands::get},

    {Reload, commands::reload},
    {FactoryReset, commands::factory_reset},

#if not SETTINGS_AUTOSAVE
    {Save, commands::save},
#endif
};

} // namespace commands

void setup() {
    espurna::terminal::add(commands::List);
}

} // namespace
} // namespace terminal
#endif

} // namespace settings
} // namespace espurna

// -----------------------------------------------------------------------------
// Key-value API
// -----------------------------------------------------------------------------

size_t settingsSize() {
    return espurna::settings::size() - espurna::settings::available();
}

espurna::settings::Keys settingsKeys() {
    return espurna::settings::sorted_keys();
}

void settingsRegisterQueryHandler(espurna::settings::query::Handler handler) {
    espurna::settings::query::internal::handlers.push_front(handler);
}

String settingsQuery(espurna::StringView key) {
    return espurna::settings::query::find(key);
}

void moveSetting(const String& from, const String& to) {
    const auto result = espurna::settings::get(from);
    if (result) {
        setSetting(to, result.ref());
        delSetting(from);
    }
}

struct SettingsKeyPair {
    espurna::settings::Key from;
    espurna::settings::Key to;
};

void moveSetting(const String& from, const String& to, size_t index) {
    const auto keys = SettingsKeyPair{
        .from = {from, index},
        .to = {to, index}
    };

    const auto result = espurna::settings::get(keys.from.value());
    if (result) {
        setSetting(keys.to, result.ref());
        delSetting(keys.from);
    }
}

void moveSettings(const String& from, const String& to) {
    for (size_t index = 0; index < 100; ++index) {
        const auto keys = SettingsKeyPair{
            .from = {from, index},
            .to = {to, index},
        };

        const auto result = espurna::settings::get(keys.from.value());
        if (!result) {
            break;
        }

        setSetting(keys.to, result.ref());
        delSetting(keys.from);
    }
}

template
bool getSetting(const espurna::settings::Key& key, bool defaultValue);

template
int getSetting(const espurna::settings::Key& key, int defaultValue);

template
long getSetting(const espurna::settings::Key& key, long defaultValue);

template
unsigned char getSetting(const espurna::settings::Key& key, unsigned char defaultValue);

template
unsigned short getSetting(const espurna::settings::Key& key, unsigned short defaultValue);

template
unsigned int getSetting(const espurna::settings::Key& key, unsigned int defaultValue);

template
unsigned long getSetting(const espurna::settings::Key& key, unsigned long defaultValue);

template
float getSetting(const espurna::settings::Key& key, float defaultValue);

template
double getSetting(const espurna::settings::Key& key, double defaultValue);

String getSetting(const String& key) {
    return std::move(espurna::settings::get(key)).get();
}

String getSetting(const __FlashStringHelper* key) {
    return getSetting(espurna::settings::Key(key));
}

String getSetting(const char* key) {
    return getSetting(espurna::settings::Key(key));
}

String getSetting(const espurna::settings::Key& key) {
    return getSetting(key, espurna::StringView(""));
}

String getSetting(const espurna::settings::Key& key, const char* defaultValue) {
    return getSetting(key, espurna::StringView(defaultValue));
}

String getSetting(const espurna::settings::Key& key, const __FlashStringHelper* defaultValue) {
    return getSetting(key, espurna::StringView(defaultValue));
}

String getSetting(const espurna::settings::Key& key, const String& defaultValue) {
    auto result = espurna::settings::get(key.value());
    if (result) {
        return std::move(result).get();
    }

    return defaultValue;
}

String getSetting(const espurna::settings::Key& key, String&& defaultValue) {
    String out;

    auto result = espurna::settings::get(key.value());
    if (result) {
        out = std::move(result).get();
    } else {
        out = std::move(defaultValue);
    }

    return out;
}

String getSetting(const espurna::settings::Key& key, espurna::StringView defaultValue) {
    String out;

    auto result = espurna::settings::get(key.value());
    if (result) {
        out = std::move(result).get();
    } else {
        out = defaultValue.toString();
    }

    return out;
}

bool delSetting(const String& key) {
    return espurna::settings::del(key);
}

bool delSetting(const espurna::settings::Key& key) {
    return delSetting(key.value());
}

bool delSetting(const char* key) {
    return delSetting(String(key));
}

bool delSetting(const __FlashStringHelper* key) {
    return delSetting(String(key));
}

bool hasSetting(const String& key) {
    return espurna::settings::has(key);
}

bool hasSetting(const espurna::settings::Key& key) {
    return hasSetting(key.value());
}

bool hasSetting(const char* key) {
    return hasSetting(String(key));
}

bool hasSetting(const __FlashStringHelper* key) {
    return hasSetting(String(key));
}

void saveSettings() {
#if not SETTINGS_AUTOSAVE
    eepromCommit();
#endif
}

void autosaveSettings() {
#if SETTINGS_AUTOSAVE
    eepromCommit();
#endif
}

void resetSettings() {
    eepromClear();
}

// -----------------------------------------------------------------------------
// API
// -----------------------------------------------------------------------------

bool settingsRestoreJson(JsonObject& data) {
    // Note: we try to match what /config generates, expect {"app":"ESPURNA",...}
    const auto& app = data[F("app")];
    if (!app.success() || !app.is<const char*>()) {
        DEBUG_MSG_P(PSTR("[SETTING] Missing 'app' key\n"));
        return false;
    }

    const auto* data_app = app.as<const char*>();
    const auto build_app = buildApp().name;
    if (build_app != data_app) {
        DEBUG_MSG_P(PSTR("[SETTING] Invalid 'app' key\n"));
        return false;
    }

    // .../config will add this key, but it is optional
    if (data[F("backup")].as<bool>()) {
        resetSettings();
    }

    // These three are just metadata, no need to actually store them
    for (auto element : data) {
        auto key = String(element.key);
        if (key.startsWith(F("app"))
            || key.startsWith(F("version"))
            || key.startsWith(F("backup")))
        {
            continue;
        }

        setSetting(std::move(key), String(element.value.as<String>()));
    }

    saveSettings();

    DEBUG_MSG_P(PSTR("[SETTINGS] Settings restored successfully\n"));
    return true;
}

bool settingsRestoreJson(char* json_string, size_t json_buffer_size) {

     // XXX: as of right now, arduinojson cannot trigger callbacks for each key individually
    // Manually separating kv pairs can allow to parse only a small chunk, since we know that there is only string type used (even with bools / ints). Can be problematic when parsing data that was not generated by us.
    // Current parsing method is limited only by keys (~sizeof(uintptr_t) bytes per key, data is not copied when string is non-const)
    DynamicJsonBuffer jsonBuffer(json_buffer_size);
    JsonObject& root = jsonBuffer.parseObject((char *) json_string);

    if (!root.success()) {
        DEBUG_MSG_P(PSTR("[SETTINGS] JSON parsing error\n"));
        return false;
    }

    return settingsRestoreJson(root);

 }

void settingsGetJson(JsonObject& root) {
    auto keys = espurna::settings::sorted_keys();
    for (const auto& key : keys) {
        auto value = getSetting(key);
        root[key] = value;
    }
}

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------

#if TERMINAL_SUPPORT

void settingsDump(const ::terminal::CommandContext& ctx,
        const espurna::settings::query::Setting* begin,
        const espurna::settings::query::Setting* end)
{
    espurna::settings::terminal::dump(ctx, begin, end);
}

void settingsDump(const ::terminal::CommandContext& ctx,
        const espurna::settings::query::IndexedSetting* begin,
        const espurna::settings::query::IndexedSetting* end, size_t index)
{
    espurna::settings::terminal::dump(ctx, begin, end, index);
}

namespace {


} // namespace
#endif

void settingsSetup() {
#if TERMINAL_SUPPORT
    espurna::settings::terminal::setup();
#endif
}
