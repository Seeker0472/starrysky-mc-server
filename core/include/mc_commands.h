#ifndef MC_COMMANDS_H
#define MC_COMMANDS_H

#include <stddef.h>
#include <stdint.h>

#define MC_COMMAND_TEXT_CAP 160u
#define MC_COMMAND_COORD_LIMIT 30000000.0
#define MC_COMMAND_DEFAULT_X 0.5
#define MC_COMMAND_DEFAULT_Y 32.0
#define MC_COMMAND_DEFAULT_Z 0.5
#define MC_COMMAND_DEFAULT_YAW 0.0f
#define MC_COMMAND_DEFAULT_PITCH 0.0f
#define MC_COMMAND_DEFAULT_TIME 6000

typedef enum {
    MC_WEATHER_CLEAR = 0,
    MC_WEATHER_RAIN = 1,
    MC_WEATHER_THUNDER = 2
} mc_weather_t;

typedef struct {
    double x;
    double y;
    double z;
    float yaw;
    float pitch;
} mc_command_position_t;

typedef struct {
    mc_command_position_t position;
    int32_t time_of_day;
    mc_weather_t weather;
} mc_command_context_t;

typedef enum {
    MC_COMMAND_RESULT_CHAT = 0,
    MC_COMMAND_RESULT_TELEPORT = 1,
    MC_COMMAND_RESULT_TIME = 2,
    MC_COMMAND_RESULT_WEATHER = 3
} mc_command_result_type_t;

typedef struct {
    mc_command_result_type_t type;
    char chat[MC_COMMAND_TEXT_CAP];
    union {
        mc_command_position_t teleport;
        int32_t time_of_day;
        mc_weather_t weather;
    } action;
} mc_command_result_t;

void mc_command_default_context(mc_command_context_t *ctx);
int mc_commands_handle_chat(const char *username,
                            const char *message,
                            const mc_command_context_t *ctx,
                            mc_command_result_t *result);

#endif
