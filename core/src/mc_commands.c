#include "mc_commands.h"

typedef struct {
    const char *ptr;
    size_t len;
} mc_command_token_t;

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} mc_text_builder_t;

#define MC_COMMAND_COORD_LIMIT_WHOLE UINT32_C(30000000)
#define MC_COMMAND_DECIMAL_MAX_FRAC_DIGITS 9u
#define MC_COMMAND_FIXED2_MAX_SCALED UINT32_C(3000000000)
#define MC_COMMAND_DOUBLE_SIGN_BIT UINT64_C(0x8000000000000000)
#define MC_COMMAND_DOUBLE_FRAC_MASK UINT64_C(0x000fffffffffffff)
#define MC_COMMAND_DOUBLE_HIDDEN_BIT UINT64_C(0x0010000000000000)
#define MC_COMMAND_DOUBLE_EXP_SHIFT 52u
#define MC_COMMAND_DOUBLE_EXP_BIAS 1023
#define MC_COMMAND_FLOAT_SIGN_BIT 0x80000000u
#define MC_COMMAND_FLOAT_FRAC_MASK 0x007fffffu
#define MC_COMMAND_FLOAT_HIDDEN_BIT 0x00800000u
#define MC_COMMAND_FLOAT_EXP_SHIFT 23u
#define MC_COMMAND_FLOAT_EXP_BIAS 127

static size_t mc_cstr_len(const char *text)
{
    size_t len = 0u;

    while (text[len] != '\0') {
        len++;
    }
    return len;
}

static void mc_builder_init(mc_text_builder_t *builder, char *buf, size_t cap)
{
    builder->buf = buf;
    builder->cap = cap;
    builder->len = 0u;
    if (cap > 0u) {
        buf[0] = '\0';
    }
}

static int mc_builder_append_char(mc_text_builder_t *builder, char ch)
{
    if (builder->len + 1u >= builder->cap) {
        return 0;
    }
    builder->buf[builder->len] = ch;
    builder->len++;
    builder->buf[builder->len] = '\0';
    return 1;
}

static int mc_builder_append_bytes(mc_text_builder_t *builder,
                                   const char *text,
                                   size_t len)
{
    size_t i;

    if (len >= builder->cap || builder->len > builder->cap - len - 1u) {
        return 0;
    }
    for (i = 0u; i < len; i++) {
        builder->buf[builder->len + i] = text[i];
    }
    builder->len += len;
    builder->buf[builder->len] = '\0';
    return 1;
}

static int mc_builder_append_cstr(mc_text_builder_t *builder, const char *text)
{
    return mc_builder_append_bytes(builder, text, mc_cstr_len(text));
}

static int mc_builder_append_u32(mc_text_builder_t *builder, uint32_t value)
{
    char digits[10];
    size_t count = 0u;

    if (value == 0u) {
        return mc_builder_append_char(builder, '0');
    }

    while (value > 0u) {
        digits[count] = (char)('0' + (value % 10u));
        value /= 10u;
        count++;
    }

    while (count > 0u) {
        count--;
        if (!mc_builder_append_char(builder, digits[count])) {
            return 0;
        }
    }
    return 1;
}

static int mc_builder_append_i32(mc_text_builder_t *builder, int32_t value)
{
    uint32_t magnitude;

    if (value < 0) {
        if (!mc_builder_append_char(builder, '-')) {
            return 0;
        }
        magnitude = (uint32_t)(-(value + 1)) + 1u;
    } else {
        magnitude = (uint32_t)value;
    }

    return mc_builder_append_u32(builder, magnitude);
}

static int mc_builder_append_fixed2_scaled(mc_text_builder_t *builder,
                                           int negative,
                                           uint32_t scaled)
{
    uint32_t whole = scaled / 100u;
    uint32_t frac = scaled % 100u;

    if (negative && scaled != 0u && !mc_builder_append_char(builder, '-')) {
        return 0;
    }
    if (!mc_builder_append_u32(builder, whole)) {
        return 0;
    }
    if (!mc_builder_append_char(builder, '.')) {
        return 0;
    }
    if (!mc_builder_append_char(builder, (char)('0' + (frac / 10u)))) {
        return 0;
    }
    return mc_builder_append_char(builder, (char)('0' + (frac % 10u)));
}

static uint64_t mc_round_shift_u64(uint64_t value, unsigned shift)
{
    uint64_t quotient;
    uint64_t remainder;
    uint64_t half;

    if (shift == 0u) {
        return value;
    }
    if (shift >= 64u) {
        return 0u;
    }

    quotient = value >> shift;
    remainder = value & ((UINT64_C(1) << shift) - 1u);
    half = UINT64_C(1) << (shift - 1u);
    if (remainder > half || (remainder == half && (quotient & 1u) != 0u)) {
        quotient++;
    }
    return quotient;
}

static int mc_binary_to_fixed2(int negative,
                               int exponent,
                               uint64_t mantissa,
                               unsigned mantissa_bits,
                               int *out_negative,
                               uint32_t *out_scaled)
{
    uint64_t base;
    uint64_t scaled;
    int shift = exponent - (int)mantissa_bits;

    if (mantissa > UINT64_MAX / 100u) {
        return 0;
    }
    base = (mantissa << 6u) + (mantissa << 5u) + (mantissa << 2u);

    if (shift >= 0) {
        if (shift >= 64 ||
            base > ((uint64_t)MC_COMMAND_FIXED2_MAX_SCALED >> (unsigned)shift)) {
            return 0;
        }
        scaled = base << (unsigned)shift;
    } else {
        scaled = mc_round_shift_u64(base, (unsigned)(-shift));
    }

    if (scaled > (uint64_t)MC_COMMAND_FIXED2_MAX_SCALED) {
        return 0;
    }
    *out_negative = negative && scaled != 0u;
    *out_scaled = (uint32_t)scaled;
    return 1;
}

static int mc_double_to_fixed2(double value, int *negative, uint32_t *scaled)
{
    union {
        double d;
        uint64_t u;
    } bits;
    uint64_t exp_bits;
    uint64_t frac;
    uint64_t mantissa;

    bits.d = value;
    exp_bits = (bits.u >> MC_COMMAND_DOUBLE_EXP_SHIFT) & 0x7ffu;
    frac = bits.u & MC_COMMAND_DOUBLE_FRAC_MASK;

    if (exp_bits == 0x7ffu) {
        return 0;
    }
    if (exp_bits == 0u) {
        *negative = 0;
        *scaled = 0u;
        return 1;
    }

    mantissa = MC_COMMAND_DOUBLE_HIDDEN_BIT | frac;
    return mc_binary_to_fixed2((bits.u & MC_COMMAND_DOUBLE_SIGN_BIT) != 0u,
                               (int)exp_bits - MC_COMMAND_DOUBLE_EXP_BIAS,
                               mantissa,
                               MC_COMMAND_DOUBLE_EXP_SHIFT,
                               negative,
                               scaled);
}

static int mc_float_to_fixed2(float value, int *negative, uint32_t *scaled)
{
    union {
        float f;
        uint32_t u;
    } bits;
    uint32_t exp_bits;
    uint32_t frac;
    uint32_t mantissa;

    bits.f = value;
    exp_bits = (bits.u >> MC_COMMAND_FLOAT_EXP_SHIFT) & 0xffu;
    frac = bits.u & MC_COMMAND_FLOAT_FRAC_MASK;

    if (exp_bits == 0xffu) {
        return 0;
    }
    if (exp_bits == 0u) {
        *negative = 0;
        *scaled = 0u;
        return 1;
    }

    mantissa = MC_COMMAND_FLOAT_HIDDEN_BIT | frac;
    return mc_binary_to_fixed2((bits.u & MC_COMMAND_FLOAT_SIGN_BIT) != 0u,
                               (int)exp_bits - MC_COMMAND_FLOAT_EXP_BIAS,
                               mantissa,
                               MC_COMMAND_FLOAT_EXP_SHIFT,
                               negative,
                               scaled);
}

static int mc_copy_chat(mc_command_result_t *result, const char *text)
{
    mc_text_builder_t builder;

    mc_builder_init(&builder, result->chat, MC_COMMAND_TEXT_CAP);
    return mc_builder_append_cstr(&builder, text);
}

static int mc_set_chat(mc_command_result_t *result, const char *text)
{
    result->type = MC_COMMAND_RESULT_CHAT;
    return mc_copy_chat(result, text);
}

static int mc_token_eq(mc_command_token_t token, const char *text)
{
    size_t len = mc_cstr_len(text);
    size_t i;

    if (token.len != len) {
        return 0;
    }
    for (i = 0u; i < len; i++) {
        if (token.ptr[i] != text[i]) {
            return 0;
        }
    }
    return 1;
}

static size_t mc_split_spaces(const char *text,
                              mc_command_token_t *tokens,
                              size_t token_cap,
                              int *too_many)
{
    size_t count = 0u;
    const char *p = text;

    *too_many = 0;
    while (*p != '\0') {
        const char *start;

        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        start = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }

        if (count < token_cap) {
            tokens[count].ptr = start;
            tokens[count].len = (size_t)(p - start);
        } else {
            *too_many = 1;
        }
        count++;
    }

    return count;
}

static double mc_decimal_to_double(int negative, uint32_t whole, uint32_t frac, uint32_t scale)
{
    union {
        double d;
        uint64_t u;
    } bits;
    uint64_t q32;
    unsigned msb = 0u;
    uint64_t mantissa;
    int exponent;
    int shift;

    static const uint32_t frac_q32_place[MC_COMMAND_DECIMAL_MAX_FRAC_DIGITS] = {
        UINT32_C(429496730),
        UINT32_C(42949673),
        UINT32_C(4294967),
        UINT32_C(429497),
        UINT32_C(42950),
        UINT32_C(4295),
        UINT32_C(430),
        UINT32_C(43),
        UINT32_C(4)
    };

    bits.u = 0u;
    q32 = (uint64_t)whole << 32u;
    if (frac != 0u) {
        uint32_t frac_value = frac;
        uint64_t frac_q32 = 0u;
        unsigned digits[MC_COMMAND_DECIMAL_MAX_FRAC_DIGITS];
        unsigned digit_count = 0u;
        unsigned i = 0u;

        while (scale > 1u && i < MC_COMMAND_DECIMAL_MAX_FRAC_DIGITS) {
            digits[digit_count++] = (unsigned)(frac_value % 10u);
            frac_value /= 10u;
            scale /= 10u;
            i++;
        }
        while (digit_count > 0u) {
            digit_count--;
            frac_q32 += (uint64_t)digits[digit_count] *
                        (uint64_t)frac_q32_place[i - digit_count - 1u];
        }
        q32 += frac_q32;
    }

    if (q32 == 0u) {
        return bits.d;
    }

    while ((q32 >> msb) > 1u) {
        msb++;
    }

    exponent = (int)msb - 32;
    shift = (int)MC_COMMAND_DOUBLE_EXP_SHIFT - (int)msb;
    if (shift >= 0) {
        mantissa = q32 << (unsigned)shift;
    } else {
        mantissa = mc_round_shift_u64(q32, (unsigned)(-shift));
        if ((mantissa >> (MC_COMMAND_DOUBLE_EXP_SHIFT + 1u)) != 0u) {
            mantissa >>= 1u;
            exponent++;
        }
    }

    bits.u = ((uint64_t)(exponent + MC_COMMAND_DOUBLE_EXP_BIAS) << MC_COMMAND_DOUBLE_EXP_SHIFT) |
             (mantissa & MC_COMMAND_DOUBLE_FRAC_MASK);
    if (negative) {
        bits.u |= MC_COMMAND_DOUBLE_SIGN_BIT;
    }
    return bits.d;
}

static int mc_parse_decimal(mc_command_token_t token, double *out, int *out_of_range)
{
    size_t i = 0u;
    int negative = 0;
    int range_error = 0;
    uint32_t whole = 0u;
    uint32_t frac = 0u;
    uint32_t scale = 1u;
    unsigned int_digits = 0u;
    unsigned frac_digits = 0u;
    int discarded_nonzero_frac = 0;

    if (token.len == 0u) {
        return 0;
    }
    *out_of_range = 0;

    if (token.ptr[i] == '-' || token.ptr[i] == '+') {
        negative = token.ptr[i] == '-';
        i++;
        if (i == token.len) {
            return 0;
        }
    }

    while (i < token.len && token.ptr[i] >= '0' && token.ptr[i] <= '9') {
        unsigned digit = (unsigned)(token.ptr[i] - '0');

        if (whole > (MC_COMMAND_COORD_LIMIT_WHOLE - digit) / 10u) {
            range_error = 1;
        } else if (!range_error) {
            whole = whole * 10u + digit;
        }
        i++;
        int_digits++;
    }

    if (int_digits == 0) {
        return 0;
    }

    if (i < token.len && token.ptr[i] == '.') {
        i++;
        while (i < token.len && token.ptr[i] >= '0' && token.ptr[i] <= '9') {
            if (!range_error && frac_digits < MC_COMMAND_DECIMAL_MAX_FRAC_DIGITS) {
                frac = frac * 10u + (uint32_t)(token.ptr[i] - '0');
                scale *= 10u;
            } else if (token.ptr[i] != '0') {
                discarded_nonzero_frac = 1;
            }
            i++;
            frac_digits++;
        }
        if (frac_digits == 0) {
            return 0;
        }
    }

    if (i != token.len) {
        return 0;
    }

    if (whole > MC_COMMAND_COORD_LIMIT_WHOLE ||
        (whole == MC_COMMAND_COORD_LIMIT_WHOLE && (frac != 0u || discarded_nonzero_frac))) {
        range_error = 1;
    }

    *out_of_range = range_error;
    if (range_error) {
        union {
            double d;
            uint64_t u;
        } zero;

        zero.u = 0u;
        *out = zero.d;
    } else {
        *out = mc_decimal_to_double(negative, whole, frac, scale);
    }
    return 1;
}

static int mc_parse_time_ticks(mc_command_token_t token, int32_t *out)
{
    size_t i;
    int32_t value = 0;

    if (token.len == 0u) {
        return 0;
    }

    for (i = 0u; i < token.len; i++) {
        char ch = token.ptr[i];

        if (ch < '0' || ch > '9') {
            return 0;
        }
        if (value <= 24000) {
            value = (int32_t)(value * 10 + (ch - '0'));
        }
    }

    *out = value;
    return 1;
}

static int mc_plain_chat(const char *username,
                         const char *message,
                         mc_command_result_t *result)
{
    mc_text_builder_t builder;

    result->type = MC_COMMAND_RESULT_CHAT;
    mc_builder_init(&builder, result->chat, MC_COMMAND_TEXT_CAP);
    return mc_builder_append_char(&builder, '<') &&
           mc_builder_append_cstr(&builder, username) &&
           mc_builder_append_cstr(&builder, "> ") &&
           mc_builder_append_cstr(&builder, message);
}

static int mc_handle_spawn(mc_command_result_t *result)
{
    result->type = MC_COMMAND_RESULT_TELEPORT;
    result->action.teleport.x = MC_COMMAND_DEFAULT_X;
    result->action.teleport.y = MC_COMMAND_DEFAULT_Y;
    result->action.teleport.z = MC_COMMAND_DEFAULT_Z;
    result->action.teleport.yaw = MC_COMMAND_DEFAULT_YAW;
    result->action.teleport.pitch = MC_COMMAND_DEFAULT_PITCH;
    return mc_copy_chat(result, "Teleported to spawn.");
}

static int mc_handle_tp(const mc_command_token_t *tokens,
                        size_t count,
                        const mc_command_context_t *ctx,
                        mc_command_result_t *result)
{
    double x;
    double y;
    double z;
    int x_out_of_range = 0;
    int y_out_of_range = 0;
    int z_out_of_range = 0;

    if (count != 4u) {
        return mc_set_chat(result, "Usage: /tp <x> <y> <z>");
    }

    if (!mc_parse_decimal(tokens[1], &x, &x_out_of_range) ||
        !mc_parse_decimal(tokens[2], &y, &y_out_of_range) ||
        !mc_parse_decimal(tokens[3], &z, &z_out_of_range)) {
        return mc_set_chat(result, "Invalid number.");
    }
    if (x_out_of_range || y_out_of_range || z_out_of_range) {
        return mc_set_chat(result, "Coordinate out of range.");
    }

    result->type = MC_COMMAND_RESULT_TELEPORT;
    result->action.teleport.x = x;
    result->action.teleport.y = y;
    result->action.teleport.z = z;
    result->action.teleport.yaw = ctx->position.yaw;
    result->action.teleport.pitch = ctx->position.pitch;
    return mc_copy_chat(result, "Teleported.");
}

static int mc_handle_pos(const mc_command_context_t *ctx,
                         mc_command_result_t *result)
{
    mc_text_builder_t builder;
    int x_negative = 0;
    int y_negative = 0;
    int z_negative = 0;
    int yaw_negative = 0;
    int pitch_negative = 0;
    uint32_t x_scaled = 0u;
    uint32_t y_scaled = 0u;
    uint32_t z_scaled = 0u;
    uint32_t yaw_scaled = 0u;
    uint32_t pitch_scaled = 0u;

    if (!mc_double_to_fixed2(ctx->position.x, &x_negative, &x_scaled) ||
        !mc_double_to_fixed2(ctx->position.y, &y_negative, &y_scaled) ||
        !mc_double_to_fixed2(ctx->position.z, &z_negative, &z_scaled) ||
        !mc_float_to_fixed2(ctx->position.yaw, &yaw_negative, &yaw_scaled) ||
        !mc_float_to_fixed2(ctx->position.pitch, &pitch_negative, &pitch_scaled)) {
        return mc_set_chat(result, "Known position unavailable.");
    }

    result->type = MC_COMMAND_RESULT_CHAT;
    mc_builder_init(&builder, result->chat, MC_COMMAND_TEXT_CAP);
    return mc_builder_append_cstr(&builder, "Known position: x=") &&
           mc_builder_append_fixed2_scaled(&builder, x_negative, x_scaled) &&
           mc_builder_append_cstr(&builder, " y=") &&
           mc_builder_append_fixed2_scaled(&builder, y_negative, y_scaled) &&
           mc_builder_append_cstr(&builder, " z=") &&
           mc_builder_append_fixed2_scaled(&builder, z_negative, z_scaled) &&
           mc_builder_append_cstr(&builder, " yaw=") &&
           mc_builder_append_fixed2_scaled(&builder, yaw_negative, yaw_scaled) &&
           mc_builder_append_cstr(&builder, " pitch=") &&
           mc_builder_append_fixed2_scaled(&builder, pitch_negative, pitch_scaled);
}

static int mc_time_result(mc_command_result_t *result, int32_t ticks)
{
    mc_text_builder_t builder;

    result->type = MC_COMMAND_RESULT_TIME;
    result->action.time_of_day = ticks;
    mc_builder_init(&builder, result->chat, MC_COMMAND_TEXT_CAP);
    return mc_builder_append_cstr(&builder, "Time set to ") &&
           mc_builder_append_i32(&builder, ticks) &&
           mc_builder_append_char(&builder, '.');
}

static int mc_handle_time(const mc_command_token_t *tokens,
                          size_t count,
                          mc_command_result_t *result)
{
    int32_t ticks = 0;

    if (count != 2u) {
        return mc_set_chat(result, "Usage: /time <day|noon|night|midnight|ticks>");
    }

    if (mc_token_eq(tokens[1], "day")) {
        return mc_time_result(result, 1000);
    }
    if (mc_token_eq(tokens[1], "noon")) {
        return mc_time_result(result, 6000);
    }
    if (mc_token_eq(tokens[1], "night")) {
        return mc_time_result(result, 13000);
    }
    if (mc_token_eq(tokens[1], "midnight")) {
        return mc_time_result(result, 18000);
    }

    if (!mc_parse_time_ticks(tokens[1], &ticks)) {
        return mc_set_chat(result, "Invalid number.");
    }
    if (ticks < 0 || ticks > 24000) {
        return mc_set_chat(result, "Time must be 0..24000.");
    }
    return mc_time_result(result, ticks);
}

static int mc_weather_result(mc_command_result_t *result,
                             mc_weather_t weather,
                             const char *name)
{
    mc_text_builder_t builder;

    result->type = MC_COMMAND_RESULT_WEATHER;
    result->action.weather = weather;
    mc_builder_init(&builder, result->chat, MC_COMMAND_TEXT_CAP);
    return mc_builder_append_cstr(&builder, "Weather set to ") &&
           mc_builder_append_cstr(&builder, name) &&
           mc_builder_append_char(&builder, '.');
}

static int mc_handle_weather(const mc_command_token_t *tokens,
                             size_t count,
                             mc_command_result_t *result)
{
    if (count != 2u) {
        return mc_set_chat(result, "Usage: /weather <clear|rain|thunder>");
    }

    if (mc_token_eq(tokens[1], "clear")) {
        return mc_weather_result(result, MC_WEATHER_CLEAR, "clear");
    }
    if (mc_token_eq(tokens[1], "rain")) {
        return mc_weather_result(result, MC_WEATHER_RAIN, "rain");
    }
    if (mc_token_eq(tokens[1], "thunder")) {
        return mc_weather_result(result, MC_WEATHER_THUNDER, "thunder");
    }
    return mc_set_chat(result, "Usage: /weather <clear|rain|thunder>");
}

void mc_command_default_context(mc_command_context_t *ctx)
{
    if (ctx == 0) {
        return;
    }

    ctx->position.x = MC_COMMAND_DEFAULT_X;
    ctx->position.y = MC_COMMAND_DEFAULT_Y;
    ctx->position.z = MC_COMMAND_DEFAULT_Z;
    ctx->position.yaw = MC_COMMAND_DEFAULT_YAW;
    ctx->position.pitch = MC_COMMAND_DEFAULT_PITCH;
    ctx->time_of_day = MC_COMMAND_DEFAULT_TIME;
    ctx->weather = MC_WEATHER_CLEAR;
}

int mc_commands_handle_chat(const char *username,
                            const char *message,
                            const mc_command_context_t *ctx,
                            mc_command_result_t *result)
{
    mc_command_token_t tokens[5];
    size_t count;
    int too_many = 0;

    if (username == 0 || message == 0 || ctx == 0 || result == 0) {
        return 0;
    }

    if (message[0] != '/') {
        return mc_plain_chat(username, message, result);
    }

    count = mc_split_spaces(message, tokens, 5u, &too_many);
    if (count == 0u || too_many) {
        return mc_set_chat(result, "Unknown command. Use /help.");
    }

    if (mc_token_eq(tokens[0], "/help")) {
        if (count != 1u) {
            return mc_set_chat(result, "Unknown command. Use /help.");
        }
        return mc_set_chat(result,
                           "Commands: /help, /spawn, /tp <x> <y> <z>, /pos, /time <day|noon|night|midnight|ticks>, /weather <clear|rain|thunder>");
    }
    if (mc_token_eq(tokens[0], "/spawn")) {
        if (count != 1u) {
            return mc_set_chat(result, "Unknown command. Use /help.");
        }
        return mc_handle_spawn(result);
    }
    if (mc_token_eq(tokens[0], "/tp")) {
        return mc_handle_tp(tokens, count, ctx, result);
    }
    if (mc_token_eq(tokens[0], "/pos")) {
        if (count != 1u) {
            return mc_set_chat(result, "Unknown command. Use /help.");
        }
        return mc_handle_pos(ctx, result);
    }
    if (mc_token_eq(tokens[0], "/time")) {
        return mc_handle_time(tokens, count, result);
    }
    if (mc_token_eq(tokens[0], "/weather")) {
        return mc_handle_weather(tokens, count, result);
    }

    return mc_set_chat(result, "Unknown command. Use /help.");
}
