#define SDL_MAIN_USE_CALLBACKS 1 /* use the callbacks instead of main() */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE 4096
#define ROM_MAX_SIZE 3584
#define ROM_GAME_ADDRESS_START 0x200
#define STEP_RATE_IN_MILLISECONDS 3
#define STEP_TIMERS_UPDATE_IN_MILLISECONDS 17
#define PIXEL_SIZE 20
#define CHIP8_DISPLAY_WIDTH 64
#define CHIP8_DISPLAY_HEIGHT 32
#define CHIP8_DISPLAY_MATRIX_SIZE (CHIP8_DISPLAY_WIDTH * CHIP8_DISPLAY_HEIGHT)
#define SDL_WINDOW_WIDTH (PIXEL_SIZE * CHIP8_DISPLAY_WIDTH)
#define SDL_WINDOW_HEIGHT (PIXEL_SIZE * CHIP8_DISPLAY_HEIGHT)

/*
 * CHIP-8 instructions are divided into broad categories by the first “nibble”,
 * or “half-byte”, which is the first hexadecimal number.
 */
#define GET_FIRST_NIBBLE(instruction) ((instruction & 0xF000) >> 12)
/*
 * The second nibble. Used to look up one of the 16 registers (VX) from V0
 * through VF.
 */
#define GET_SECOND_NIBBLE(instruction) ((instruction & 0xF00) >> 8)
/*
 * The third nibble. Also used to look up one of the 16 registers (VY) from V0
 * through VF.
 */
#define GET_THIRD_NIBBLE(instruction) ((instruction & 0xF0) >> 4)
/*
 * The fourth nibble. A 4-bit number.
 */
#define GET_FOURTH_NIBBLE(instruction) (instruction & 0xF)
/*
 * The second byte (third and fourth nibbles). An 8-bit immediate number.
 */
#define GET_THIRD_AND_FORTH_NIBBLES(instruction) (instruction & 0xFF)
/*
 * The second, third and fourth nibbles. A 12-bit immediate memory address.
 */
#define GET_SECOND_THIRD_AND_FOURTH_NIBBLES(instruction) (instruction & 0xFFF)

typedef struct
{
    // Memory
    uint8_t RAM[RAM_SIZE];
    // Points at the current instruction in memory
    uint16_t PC;
    // Points at locations in memory
    uint16_t I;
    // Stack
    uint16_t stack[16];
    // Stack pointer
    uint8_t SP;
    // Decremented at a rate of 60 Hz (60 times per second) until it reaches 0
    uint8_t delay_timer;
    // Sound timer which functions like the delay timer, but which also
    // gives off a beeping sound as long as it’s not 0
    uint8_t sound_timer;
    // 64 x 32 black and white screen cells
    uint64_t display_cells[CHIP8_DISPLAY_MATRIX_SIZE / 64U];
    // General-purpose variable registers numbered 0 through F hexadecimal,
    // ie. 0 through 15 in decimal, called V0 through VF
    uint8_t V[16];
    // Keyboard mapping
    // Index of button - Button of real keyboard
    uint8_t keyboard_keys[16];
} CHIP8Context;

typedef struct
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_AudioStream *stream;
    // TODO: Maybe move audio fields to another struct
    Uint8 *wav_data;
    Uint32 wav_data_len;
    CHIP8Context chip8_context;
    bool need_redraw;
    Uint64 last_step;
    Uint64 last_timer_update;
    const bool *keyboard_state;
} AppState;

const uint8_t keys[] = {
    SDL_SCANCODE_X,
    SDL_SCANCODE_1,
    SDL_SCANCODE_2,
    SDL_SCANCODE_3,
    SDL_SCANCODE_Q,
    SDL_SCANCODE_W,
    SDL_SCANCODE_E,
    SDL_SCANCODE_A,
    SDL_SCANCODE_S,
    SDL_SCANCODE_D,
    SDL_SCANCODE_Z,
    SDL_SCANCODE_C,
    SDL_SCANCODE_4,
    SDL_SCANCODE_R,
    SDL_SCANCODE_F,
    SDL_SCANCODE_V,
};

void read_rom_file(int argc, char *argv[], AppState *appstate)
{
    char *rom_file_address = argv[2];
    FILE *fp;

    for (int i = 1; i < argc; i += 1)
    {
        if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--rom") == 0) &&
            (i + 1) < argc)
        {
            rom_file_address = argv[++i];
        }
        else
        {
            puts("You need to provide all correct program arguments!");
        }
    }

    if ((fp = fopen(rom_file_address, "r")) == NULL)
    {
        perror(rom_file_address);
    }

    /* Calculate file length */
    fseek(fp, 0, SEEK_END);
    int rom_file_size = ftell(fp);
    rewind(fp);

    if (rom_file_size > ROM_MAX_SIZE)
    {
        puts("Your rom is too big for CHIP-8!");
        fclose(fp);
    }

    fread(&appstate->chip8_context.RAM[0x200], 1, rom_file_size, fp);
    fclose(fp);
}

/* This function runs once at startup. */
SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    char *wav_path = NULL;
    SDL_AudioSpec spec;

    SDL_SetAppMetadata("Chip-8 Emulator", "0.0.1", "com.example.emulator");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
    {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    AppState *as = (AppState *)SDL_calloc(1, sizeof(AppState));
    if (!as)
    {
        return SDL_APP_FAILURE;
    }
    *appstate = as;

    as->need_redraw = false;
    as->chip8_context.PC = ROM_GAME_ADDRESS_START;
    memcpy(as->chip8_context.keyboard_keys, keys, sizeof(keys));
    SDL_zeroa(as->chip8_context.display_cells);
    int num_keys = 0;
    as->keyboard_state = SDL_GetKeyboardState(&num_keys); 

    SDL_asprintf(&wav_path, "%sassets/441634__xtrgamr__asynth.wav", SDL_GetBasePath());
    if (!SDL_LoadWAV(wav_path, &spec, &as->wav_data, &as->wav_data_len))
    {
        SDL_Log("Couldn't load .wav file: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_free(wav_path);

    // Create audio stream
    as->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!as->stream)
    {
        SDL_Log("Couldn't create audio stream: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_ResumeAudioStreamDevice(as->stream);

    if (!SDL_CreateWindowAndRenderer("examples/emulator/chip-8", SDL_WINDOW_WIDTH,
                                     SDL_WINDOW_HEIGHT, 0, &as->window,
                                     &as->renderer))
    {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    read_rom_file(argc, argv, as);

    as->last_step = SDL_GetTicks();
    as->last_timer_update = SDL_GetTicks();

    return SDL_APP_CONTINUE;
}

/* This function runs when a new event (mouse input, keypresses, etc) occurs. */
SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    if (event->type == SDL_EVENT_QUIT)
    {
        return SDL_APP_SUCCESS; /* end the program, reporting success to the OS. */
    }
    return SDL_APP_CONTINUE;
}

uint16_t fetch_instruction(AppState *appstate)
{
    uint16_t instruction =
        ((appstate->chip8_context.RAM[appstate->chip8_context.PC] << 8) |
         appstate->chip8_context.RAM[appstate->chip8_context.PC + 1]);
    appstate->chip8_context.PC += 0x2;
    return instruction;
}

int get_display_cell(AppState *appstate, short row, short col)
{
    int shift = CHIP8_DISPLAY_WIDTH - col - 1;
    return (appstate->chip8_context.display_cells[row] >> shift & 0x1);
}

void stack_push_instruction(uint16_t instruction, AppState *appstate)
{
    if (appstate->chip8_context.SP >= 16)
    {
        exit(1);
    }
    appstate->chip8_context.stack[++appstate->chip8_context.SP] = instruction;
}

uint16_t stack_pop_instruction(AppState *appstate)
{
    if (appstate->chip8_context.SP > 0)
    {
        return appstate->chip8_context.stack[appstate->chip8_context.SP--];
    }
}

void decode_instruction(uint16_t instruction, char **message,
                        AppState *appstate)
{
    // Get first nibble (to decode) from instruction using `bit mask` and `and`
    uint8_t first_nibble = GET_FIRST_NIBBLE(instruction);
    uint8_t second_nibble = GET_SECOND_NIBBLE(instruction);
    uint8_t third_nibble = GET_THIRD_NIBBLE(instruction);
    uint8_t fourth_nibble = GET_FOURTH_NIBBLE(instruction);
    uint8_t third_and_fourth_nibbles = GET_THIRD_AND_FORTH_NIBBLES(instruction);
    uint16_t second_third_and_fourth_nibbles =
        GET_SECOND_THIRD_AND_FOURTH_NIBBLES(instruction);

    switch (first_nibble)
    {
    case 0x0:
        switch (fourth_nibble)
        {
        case 0x0:
            SDL_memset(appstate->chip8_context.display_cells, 0,
                       sizeof(appstate->chip8_context.display_cells));
            appstate->need_redraw = true;
            snprintf(*message, 256, "Clear the display");
            break;
        case 0xE:
            appstate->chip8_context.PC = stack_pop_instruction(appstate);
            snprintf(*message, 256, "Returning from a subroutine");
            break;
        }
        break;
    case 0x1:
        appstate->chip8_context.PC = second_third_and_fourth_nibbles;
        snprintf(*message, 256, "Jump to address %x",
                 second_third_and_fourth_nibbles);
        break;
    case 0x2:
        stack_push_instruction(appstate->chip8_context.PC, appstate);
        appstate->chip8_context.PC = second_third_and_fourth_nibbles;
        snprintf(*message, 256, "Call the subroutine at memory location %x",
                 second_third_and_fourth_nibbles);
        break;
    case 0x3:
        if (appstate->chip8_context.V[second_nibble] == third_and_fourth_nibbles)
        {
            appstate->chip8_context.PC += 0x2;
        }
        snprintf(*message,
                 256,
                 "Skip next instruction if V%x is equal to %x",
                 second_nibble,
                 third_and_fourth_nibbles);
        break;
    case 0x4:
        if (appstate->chip8_context.V[second_nibble] != third_and_fourth_nibbles)
        {
            appstate->chip8_context.PC += 0x2;
        }
        snprintf(*message,
                 256,
                 "Skip next instruction if V%x is not equal to %x",
                 second_nibble,
                 third_and_fourth_nibbles);
        break;
    case 0x5:
        if (appstate->chip8_context.V[second_nibble] == appstate->chip8_context.V[third_nibble])
        {
            appstate->chip8_context.PC += 0x2;
        }
        snprintf(*message,
                 256,
                 "Skip next instruction if V%x is equal to V%x",
                 second_nibble,
                 third_nibble);
        break;
    case 0x6:
        appstate->chip8_context.V[second_nibble] = third_and_fourth_nibbles;
        snprintf(*message, 256, "Set value %x to register V%x",
                 third_and_fourth_nibbles, second_nibble);
        break;
    case 0x7:
        appstate->chip8_context.V[second_nibble] += third_and_fourth_nibbles;
        snprintf(*message, 256, "Add value %x to register V%x",
                 third_and_fourth_nibbles, second_nibble);
        break;
    case 0x8:
        switch (fourth_nibble) {
            case 0x0:
                appstate->chip8_context.V[second_nibble] = appstate->chip8_context.V[third_nibble]; 
                snprintf(*message, 256, "V%x is set to the value of V%x",
                         second_nibble, third_nibble);
                break;
            case 0x1:
                appstate->chip8_context.V[second_nibble] |= appstate->chip8_context.V[third_nibble]; 
                snprintf(*message, 256, "V%x is set to the bitwise OR of V%x and V%x",
                         second_nibble, second_nibble, third_nibble);
                break;
            case 0x2:
                appstate->chip8_context.V[second_nibble] &= appstate->chip8_context.V[third_nibble]; 
                snprintf(*message, 256, "V%x is set to the bitwise AND of V%x and V%x",
                         second_nibble, second_nibble, third_nibble);
                break;
            case 0x3:
                appstate->chip8_context.V[second_nibble] ^= appstate->chip8_context.V[third_nibble]; 
                snprintf(*message, 256, "V%x is set to the bitwise XOR of V%x and V%x",
                         second_nibble, second_nibble, third_nibble);
                break;
            case 0x4:
                uint16_t add_result = appstate->chip8_context.V[second_nibble] + appstate->chip8_context.V[third_nibble];
                appstate->chip8_context.V[second_nibble] = (uint8_t)add_result;
                if (add_result > 255)
                    appstate->chip8_context.V[15] = 1;   
                else
                    appstate->chip8_context.V[15] = 0;
                snprintf(*message, 256, "V%x is set to the value of V%x plus the value of V%x",
                         second_nibble, second_nibble, third_nibble);
                break;
            case 0x5:
                bool is_lower = appstate->chip8_context.V[second_nibble] >= appstate->chip8_context.V[third_nibble];
                appstate->chip8_context.V[second_nibble] -= appstate->chip8_context.V[third_nibble];
                if (is_lower)
                    appstate->chip8_context.V[15] = 1;
                else 
                    appstate->chip8_context.V[15] = 0;
                snprintf(*message, 256, "V%x is set to the value of V%x - V%x",
                         second_nibble, second_nibble, third_nibble);
                break;
            case 0x6:
                uint8_t least_significant_bit;
                if ((appstate->chip8_context.V[second_nibble] & 0x1) == 0x1) {
                    least_significant_bit = 1;
                }
                else {
                    least_significant_bit = 0;
                }
                appstate->chip8_context.V[second_nibble] >>= 1;
                appstate->chip8_context.V[15] = least_significant_bit;
                snprintf(*message, 256, "Right shift V%x", second_nibble);
                break;
            case 0x7:
                is_lower = appstate->chip8_context.V[third_nibble] >= appstate->chip8_context.V[second_nibble];
                appstate->chip8_context.V[second_nibble] = appstate->chip8_context.V[third_nibble] - appstate->chip8_context.V[second_nibble];
                if (is_lower)
                    appstate->chip8_context.V[15] = 1;
                else
                    appstate->chip8_context.V[15] = 0;
                snprintf(*message, 256, "V%x is set to the value of V%x plus the value of V%x",
                         second_nibble, third_nibble, second_nibble);
                break;
            case 0xE:
                uint8_t most_significant_bit;
                if ((appstate->chip8_context.V[second_nibble] & 0x80) == 0x80) {
                    most_significant_bit = 1;
                }
                else {
                    most_significant_bit = 0;
                }
                appstate->chip8_context.V[second_nibble] <<= 1;
                appstate->chip8_context.V[15] = most_significant_bit;
                snprintf(*message, 256, "Left shift V%x", second_nibble);
                break;
        }
        break;
    case 0x9:
        if (appstate->chip8_context.V[second_nibble] != appstate->chip8_context.V[third_nibble])
        {
            appstate->chip8_context.PC += 0x2;
        }
        snprintf(*message,
                 256,
                 "Skip next instruction if V%x is not equal to V%x",
                 second_nibble,
                 third_nibble);
        break;
    case 0xA:
        appstate->chip8_context.I = second_third_and_fourth_nibbles;
        snprintf(*message, 256, "Set address %x to register I",
                 second_third_and_fourth_nibbles);
        break;
    case 0xD:
        uint16_t x_coord = appstate->chip8_context.V[second_nibble];
        uint16_t y_coord = appstate->chip8_context.V[third_nibble];
        uint8_t n = fourth_nibble;
        appstate->chip8_context.V[0xF] = 0;

        for (short y = 0; y < n; y++)
        {
            uint8_t sprite_data = appstate->chip8_context.RAM[appstate->chip8_context.I + y];
            uint64_t *display_row = &appstate->chip8_context.display_cells[y_coord + y];
            for (short x = 0; x < 8; x++)
            {
                int sprite_bit_value = (sprite_data >> (7 - x)) & 0x1;
                int bit_pos = CHIP8_DISPLAY_WIDTH - 1 - (x_coord + x);
                *display_row ^= ((uint64_t)sprite_bit_value << bit_pos);
            }
        }
        appstate->need_redraw = true;
        snprintf(
            *message, 256,
            "Display %x-byte sprite starting at memory location I at (V%x, V%x)",
            fourth_nibble, second_nibble, third_nibble);
        break;
    case 0xE:
        switch (third_and_fourth_nibbles)
        {
        case 0x9E:
            uint8_t key_scancode = appstate->chip8_context.keyboard_keys[second_nibble];
            if (appstate->keyboard_state[key_scancode]) {
                appstate->chip8_context.PC += 2;
                snprintf(
                    *message,
                    256,
                    "Key %zx is pressed, next instruction skipped.",
                    key_scancode
                );
                break;
            }
            break;
        case 0xA1:
            key_scancode = appstate->chip8_context.keyboard_keys[second_nibble];
            if (!appstate->keyboard_state[key_scancode]) {
                appstate->chip8_context.PC += 2;
                snprintf(
                    *message,
                    256,
                    "Key %zx is NOT pressed, next instruction skipped.",
                    key_scancode
                );
                break;
            }
            break;
        }
        break;
    case 0xF:
        switch (third_and_fourth_nibbles)
        {
        case 0x07:
            appstate->chip8_context.V[second_nibble] = appstate->chip8_context.delay_timer;
            snprintf(*message, 256, "Set delay timer to V%x", second_nibble);
            break;
        case 0x15:
            appstate->chip8_context.delay_timer = appstate->chip8_context.V[second_nibble];
            snprintf(*message, 256, "Set V%x to delay timer", second_nibble);
            break;
        case 0x18:
            appstate->chip8_context.sound_timer = appstate->chip8_context.V[second_nibble];
            snprintf(*message, 256, "Set V%x to sound timer", second_nibble);
            break;
        case 0x0A:
            bool is_key_pressed = false;
            for (size_t key_index = 0; key_index < 16; key_index++) {
                uint8_t cur_scancode = appstate->chip8_context.keyboard_keys[key_index];
                if (appstate->keyboard_state[cur_scancode]) {
                    snprintf(*message, 256, "Key %zx is pressed, number putted in V%x.", key_index, second_nibble);
                    appstate->chip8_context.V[second_nibble] = (uint8_t)key_index;
                    is_key_pressed = true;
                    break;
                }
            }             
            if (!is_key_pressed) {
                appstate->chip8_context.PC -= 2;
                snprintf(*message, 256, "Wait for key input, put key number into V%x",
                         second_nibble);
                break;
            }
            break;
        case 0x33:
            // We need to split VX number to three decimal digits
            uint8_t v_number = appstate->chip8_context.V[second_nibble];
            uint8_t number;
            uint8_t decimal_base = 10;
            for (int offset = 2; offset >= 0; offset--) {
                number = v_number % decimal_base;
                v_number /= decimal_base;
                appstate->chip8_context.RAM[appstate->chip8_context.I + offset] = number;
            }
            snprintf(
                *message,
                256,
                "Store BCD representation of V%x in memory locations I, I+1, and I+2",
                second_nibble
            );
            break;
        case 0x55:
            for (int register_number = 0; register_number <= second_nibble; register_number++) {
                appstate->chip8_context.RAM[appstate->chip8_context.I + register_number] = appstate->chip8_context.V[register_number]; 
            }
            snprintf(
                *message,
                256,
                "Store registers V0 through V%x in memory starting at location I",
                second_nibble
            );
            break;
        case 0x65:
            for (int register_number = 0; register_number <= second_nibble; register_number++) {
                appstate->chip8_context.V[register_number] = appstate->chip8_context.RAM[appstate->chip8_context.I + register_number]; 
            }
            snprintf(
                *message,
                256,
                "Read registers V0 through V%x from memory starting at location I",
                second_nibble
            );
            break;
        case 0x1E:
            appstate->chip8_context.I += appstate->chip8_context.V[second_nibble];
            snprintf(*message, 256, "Set I = I + V%x",
                     second_nibble);
        }
        break;
    default:
        snprintf(*message, 256, "Not an instruction");
        break;
    }
}

static void set_rect_xy_(SDL_FRect *r, short x, short y)
{
    r->x = (float)(x * PIXEL_SIZE);
    r->y = (float)(y * PIXEL_SIZE);
}

void draw_screen(AppState *appstate)
{
    SDL_FRect r;
    r.w = r.h = PIXEL_SIZE;
    unsigned i;
    unsigned j;
    int display_cell;

    SDL_SetRenderDrawColor(appstate->renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(appstate->renderer);

    for (j = 0; j < CHIP8_DISPLAY_HEIGHT; j++)
    {
        for (i = 0; i < CHIP8_DISPLAY_WIDTH; i++)
        {
            display_cell = get_display_cell(appstate, j, i);
            if (display_cell == 0)
            {
                SDL_SetRenderDrawColor(appstate->renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
            }
            else
            {
                SDL_SetRenderDrawColor(appstate->renderer, 255, 255, 255, SDL_ALPHA_OPAQUE);
            }
            set_rect_xy_(&r, i, j);
            SDL_RenderFillRect(appstate->renderer, &r);
        }
    }

    SDL_RenderPresent(appstate->renderer);
}

/* This function runs once per frame, and is the heart of the program. */
SDL_AppResult SDL_AppIterate(void *appstate)
{
    AppState *as = (AppState *)appstate;
    const Uint64 now = SDL_GetTicks();

    char *message = malloc(256);

    SDL_PumpEvents();

    if ((now - as->last_timer_update) >= STEP_TIMERS_UPDATE_IN_MILLISECONDS)
    {
        // update timers
        if (as->chip8_context.delay_timer > 0)
        {
            --as->chip8_context.delay_timer;
        }
        if (as->chip8_context.sound_timer > 0)
        {
            SDL_ResumeAudioStreamDevice(as->stream);
            if (SDL_GetAudioStreamQueued(as->stream) < (int)as->wav_data_len)
            {
                SDL_PutAudioStreamData(as->stream, as->wav_data, as->wav_data_len);
            }
            --as->chip8_context.sound_timer;
        }
        as->last_timer_update += STEP_TIMERS_UPDATE_IN_MILLISECONDS;
    }

    while ((now - as->last_step) >= STEP_RATE_IN_MILLISECONDS)
    {
        // do emulator step
        uint16_t cur_instruction = fetch_instruction(as);
        decode_instruction(cur_instruction, &message, as);
        // printf("%04x: %s\n", cur_instruction, message);
        if (as->need_redraw)
        {
            draw_screen(as);
            as->need_redraw = false;
        }

        as->last_step += STEP_RATE_IN_MILLISECONDS;
    }

    free(message);

    return SDL_APP_CONTINUE;
}

/* This function runs once at shutdown. */
void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    if (appstate != NULL)
    {
        AppState *as = (AppState *)appstate;
        SDL_DestroyRenderer(as->renderer);
        SDL_DestroyWindow(as->window);
        SDL_free(as->wav_data);
        SDL_free(as);
    }
}
