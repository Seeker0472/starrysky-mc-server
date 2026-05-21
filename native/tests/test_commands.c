#include <string.h>
#include "mc_commands.h"

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)
#define ASSERT_STREQ(a, b) do { if (strcmp((a), (b)) != 0) return 1; } while (0)

static int double_close(double actual, double expected)
{
    double diff = actual > expected ? actual - expected : expected - actual;

    return diff < 0.000001;
}

static int handle(const char *message,
                  const mc_command_context_t *ctx,
                  mc_command_result_t *result)
{
    return mc_commands_handle_chat("player1", message, ctx, result);
}

static int test_plain_chat(void)
{
    mc_command_context_t ctx;
    mc_command_result_t result;

    mc_command_default_context(&ctx);
    ASSERT_TRUE(handle("hello", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_CHAT);
    ASSERT_STREQ(result.chat, "<player1> hello");
    return 0;
}

static int test_help(void)
{
    mc_command_context_t ctx;
    mc_command_result_t result;

    mc_command_default_context(&ctx);
    ASSERT_TRUE(handle("/help", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_CHAT);
    ASSERT_STREQ(result.chat,
                 "Commands: /help, /spawn, /tp <x> <y> <z>, /pos, /time <day|noon|night|midnight|ticks>, /weather <clear|rain|thunder>");
    return 0;
}

static int test_spawn(void)
{
    mc_command_context_t ctx;
    mc_command_result_t result;

    mc_command_default_context(&ctx);
    ASSERT_TRUE(handle("/spawn", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_TELEPORT);
    ASSERT_EQ(result.action.teleport.x, MC_COMMAND_DEFAULT_X);
    ASSERT_EQ(result.action.teleport.y, MC_COMMAND_DEFAULT_Y);
    ASSERT_EQ(result.action.teleport.z, MC_COMMAND_DEFAULT_Z);
    ASSERT_EQ(result.action.teleport.yaw, MC_COMMAND_DEFAULT_YAW);
    ASSERT_EQ(result.action.teleport.pitch, MC_COMMAND_DEFAULT_PITCH);
    ASSERT_STREQ(result.chat, "Teleported to spawn.");
    return 0;
}

static int test_tp_success(void)
{
    mc_command_context_t ctx;
    mc_command_result_t result;

    mc_command_default_context(&ctx);
    ctx.position.yaw = 90.0f;
    ctx.position.pitch = -20.0f;
    ASSERT_TRUE(handle("/tp 1.5 40 -2", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_TELEPORT);
    ASSERT_TRUE(double_close(result.action.teleport.x, 1.5));
    ASSERT_TRUE(double_close(result.action.teleport.y, 40.0));
    ASSERT_TRUE(double_close(result.action.teleport.z, -2.0));
    ASSERT_EQ(result.action.teleport.yaw, 90.0f);
    ASSERT_EQ(result.action.teleport.pitch, -20.0f);
    ASSERT_STREQ(result.chat, "Teleported.");

    ASSERT_TRUE(handle("/tp 1.23 1.05 0.000000001", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_TELEPORT);
    ASSERT_TRUE(double_close(result.action.teleport.x, 1.23));
    ASSERT_TRUE(double_close(result.action.teleport.y, 1.05));
    ASSERT_TRUE(double_close(result.action.teleport.z, 0.000000001));
    return 0;
}

static int test_tp_errors(void)
{
    mc_command_context_t ctx;
    mc_command_result_t result;

    mc_command_default_context(&ctx);

    ASSERT_TRUE(handle("/tp 1 2", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_CHAT);
    ASSERT_STREQ(result.chat, "Usage: /tp <x> <y> <z>");

    ASSERT_TRUE(handle("/tp 1 nope 3", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_CHAT);
    ASSERT_STREQ(result.chat, "Invalid number.");

    ASSERT_TRUE(handle("/tp 30000001 2 3", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_CHAT);
    ASSERT_STREQ(result.chat, "Coordinate out of range.");

    ASSERT_TRUE(handle("/tp 30000000.0000000001 2 3", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_CHAT);
    ASSERT_STREQ(result.chat, "Coordinate out of range.");
    return 0;
}

static int test_pos(void)
{
    mc_command_context_t ctx;
    mc_command_result_t result;

    mc_command_default_context(&ctx);
    ctx.position.x = 12.50;
    ctx.position.y = 33.00;
    ctx.position.z = -4.25;
    ctx.position.yaw = 45.00f;
    ctx.position.pitch = 10.00f;

    ASSERT_TRUE(handle("/pos", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_CHAT);
    ASSERT_STREQ(result.chat,
                 "Known position: x=12.50 y=33.00 z=-4.25 yaw=45.00 pitch=10.00");
    return 0;
}

static double test_nan_value(void)
{
    volatile double zero = 0.0;

    return zero / zero;
}

static int assert_pos_unavailable(const mc_command_context_t *ctx)
{
    mc_command_result_t result;

    ASSERT_TRUE(handle("/pos", ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_CHAT);
    ASSERT_STREQ(result.chat, "Known position unavailable.");
    return 0;
}

static int test_pos_invalid_values(void)
{
    mc_command_context_t ctx;

    mc_command_default_context(&ctx);
    ctx.position.x = 1000000000000000000000000000000.0;
    ASSERT_TRUE(assert_pos_unavailable(&ctx) == 0);

    mc_command_default_context(&ctx);
    ctx.position.pitch = (float)test_nan_value();
    ASSERT_TRUE(assert_pos_unavailable(&ctx) == 0);
    return 0;
}

static int assert_time_command(const char *message, int32_t ticks)
{
    mc_command_context_t ctx;
    mc_command_result_t result;
    char expected[32];
    const char prefix[] = "Time set to ";
    size_t len = 0u;
    int32_t value = ticks;
    char digits[12];
    size_t digit_count = 0u;

    mc_command_default_context(&ctx);
    ASSERT_TRUE(handle(message, &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_TIME);
    ASSERT_EQ(result.action.time_of_day, ticks);

    while (prefix[len] != '\0') {
        expected[len] = prefix[len];
        len++;
    }
    if (value == 0) {
        digits[digit_count++] = '0';
    }
    while (value > 0) {
        digits[digit_count++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (digit_count > 0u) {
        expected[len++] = digits[--digit_count];
    }
    expected[len++] = '.';
    expected[len] = '\0';
    ASSERT_STREQ(result.chat, expected);
    return 0;
}

static int test_time_success(void)
{
    mc_command_context_t ctx;
    mc_command_result_t result;

    ASSERT_TRUE(assert_time_command("/time day", 1000) == 0);
    ASSERT_TRUE(assert_time_command("/time noon", 6000) == 0);
    ASSERT_TRUE(assert_time_command("/time night", 13000) == 0);
    ASSERT_TRUE(assert_time_command("/time midnight", 18000) == 0);

    mc_command_default_context(&ctx);
    ASSERT_TRUE(handle("/time 24000", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_TIME);
    ASSERT_EQ(result.action.time_of_day, 24000);
    ASSERT_STREQ(result.chat, "Time set to 24000.");
    return 0;
}

static int test_time_errors(void)
{
    mc_command_context_t ctx;
    mc_command_result_t result;

    mc_command_default_context(&ctx);

    ASSERT_TRUE(handle("/time", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_CHAT);
    ASSERT_STREQ(result.chat, "Usage: /time <day|noon|night|midnight|ticks>");

    ASSERT_TRUE(handle("/time nope", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_CHAT);
    ASSERT_STREQ(result.chat, "Invalid number.");

    ASSERT_TRUE(handle("/time 24001", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_CHAT);
    ASSERT_STREQ(result.chat, "Time must be 0..24000.");
    return 0;
}

static int assert_weather_command(const char *message,
                                  mc_weather_t weather,
                                  const char *chat)
{
    mc_command_context_t ctx;
    mc_command_result_t result;

    mc_command_default_context(&ctx);
    ASSERT_TRUE(handle(message, &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_WEATHER);
    ASSERT_EQ(result.action.weather, weather);
    ASSERT_STREQ(result.chat, chat);
    return 0;
}

static int test_weather_success(void)
{
    ASSERT_TRUE(assert_weather_command("/weather clear",
                                       MC_WEATHER_CLEAR,
                                       "Weather set to clear.") == 0);
    ASSERT_TRUE(assert_weather_command("/weather rain",
                                       MC_WEATHER_RAIN,
                                       "Weather set to rain.") == 0);
    ASSERT_TRUE(assert_weather_command("/weather thunder",
                                       MC_WEATHER_THUNDER,
                                       "Weather set to thunder.") == 0);
    return 0;
}

static int test_weather_errors(void)
{
    mc_command_context_t ctx;
    mc_command_result_t result;

    mc_command_default_context(&ctx);

    ASSERT_TRUE(handle("/weather", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_CHAT);
    ASSERT_STREQ(result.chat, "Usage: /weather <clear|rain|thunder>");

    ASSERT_TRUE(handle("/weather snow", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_CHAT);
    ASSERT_STREQ(result.chat, "Usage: /weather <clear|rain|thunder>");
    return 0;
}

static int test_unknown_command(void)
{
    mc_command_context_t ctx;
    mc_command_result_t result;

    mc_command_default_context(&ctx);
    ASSERT_TRUE(handle("/give dirt", &ctx, &result));
    ASSERT_EQ(result.type, MC_COMMAND_RESULT_CHAT);
    ASSERT_STREQ(result.chat, "Unknown command. Use /help.");
    return 0;
}

int test_commands(void)
{
    ASSERT_TRUE(test_plain_chat() == 0);
    ASSERT_TRUE(test_help() == 0);
    ASSERT_TRUE(test_spawn() == 0);
    ASSERT_TRUE(test_tp_success() == 0);
    ASSERT_TRUE(test_tp_errors() == 0);
    ASSERT_TRUE(test_pos() == 0);
    ASSERT_TRUE(test_pos_invalid_values() == 0);
    ASSERT_TRUE(test_time_success() == 0);
    ASSERT_TRUE(test_time_errors() == 0);
    ASSERT_TRUE(test_weather_success() == 0);
    ASSERT_TRUE(test_weather_errors() == 0);
    ASSERT_TRUE(test_unknown_command() == 0);
    return 0;
}
