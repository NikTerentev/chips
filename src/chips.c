/*
 * Use the callbacks instead of main()
 */
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NANOSECONDS_IN_SECOND     1000000000ULL
#define ROM_GAME_ADDRESS_START    0x200
#define RAM_SIZE                  4096
#define ROM_MAX_SIZE              3584
#define INSTRUCTIONS_PER_FRAME    11
#define FRAMES_PER_SECOND         60
#define CHIP8_DISPLAY_WIDTH       64
#define CHIP8_DISPLAY_HEIGHT      32
#define PIXEL_SIZE                20
#define FONT_SIZE                 16
#define FONT_BYTES                5

#define CHIP8_DISPLAY_MATRIX_SIZE (CHIP8_DISPLAY_WIDTH * CHIP8_DISPLAY_HEIGHT)
#define SDL_WINDOW_HEIGHT         (PIXEL_SIZE * CHIP8_DISPLAY_HEIGHT)
#define SDL_WINDOW_WIDTH          (PIXEL_SIZE * CHIP8_DISPLAY_WIDTH)

/*
 * Get first nibble from instruction.
 *
 * CHIP-8 instructions are divided into broad categories by the first "nibble",
 * or "half-byte", which is the first hexadecimal number.
 */
#define GET_FIRST_NIBBLE(instruction) ((instruction & 0xF000) >> 12)
/*
 * Get second, third and fourth nibbles (a 12-bit immediate memory address).
 */
#define GET_SECOND_THIRD_AND_FOURTH_NIBBLES(instruction) (instruction & 0xFFF)
/*
 * Get second nibble - used to look up one of the 16 registers (VX) from V0
 * through VF.
 */
#define GET_SECOND_NIBBLE(instruction)           ((instruction & 0xF00) >> 8)
/*
 * Get third nibble - used to look up one of the 16 registers (VY) from V0
 * through VF.
 */
#define GET_THIRD_NIBBLE(instruction)            ((instruction & 0xF0) >> 4)
/*
 * Get second byte (third and fourth nibbles) - an 8-bit immediate number.
 */
#define GET_THIRD_AND_FORTH_NIBBLES(instruction) (instruction & 0xFF)
/*
 * Get fourth nibble - a 4-bit number.
 */
#define GET_FOURTH_NIBBLE(instruction)           (instruction & 0xF)

typedef struct {
    /* 64 x 32 black and white screen cells */
    Uint64 display_cells[CHIP8_DISPLAY_MATRIX_SIZE / 64U];
    /*
    Keyboard mapping
    Index of button - Button of real keyboard
    */
    Uint8  keyboard_keys[16];
    /* Memory */
    Uint8  RAM[RAM_SIZE];
    /*
     * Decremented at a rate of 60 Hz (60 times per second) until it reaches 0
     */
    Uint8  delay_timer;
    /*
    Sound timer which functions like the delay timer, but which also
    gives off a beeping sound as long as it’s not 0
    */
    Uint8  sound_timer;
    /* Prevent rendering more than once per frame */
    bool   vblank_sync;
    /* Stack */
    Uint16 stack[16];
    /*
    General-purpose variable registers numbered 0 through F hexadecimal,
    ie. 0 through 15 in decimal, called V0 through VF
    */
    Uint8  V[16];
    /* Points at the current instruction in memory */
    Uint16 PC;
    /* Stack pointer */
    Uint8  SP;
    /* Points at locations in memory */
    Uint16 I;
} CHIP8Context;

typedef struct {
    Uint64           nanoseconds_per_frame;
    const bool      *keyboard_state;
    bool             stop_execution;
    char            *rom_file_path;
    CHIP8Context     chip8_context;
    Uint32           wav_data_len;
    bool             need_redraw;
    bool             enable_logs;
    SDL_Renderer    *renderer;
    Uint8           *wav_data;
    SDL_Window      *window;
    SDL_AudioStream *stream;
    Uint8            fps;
    Uint8            ipf;
} AppState;

const Uint8 keys[] = {
    SDL_SCANCODE_X, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3,
    SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_A,
    SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_Z, SDL_SCANCODE_C,
    SDL_SCANCODE_4, SDL_SCANCODE_R, SDL_SCANCODE_F, SDL_SCANCODE_V,
};

Uint8 DEFALT_FONT[FONT_SIZE][FONT_BYTES] = {
    /* 0 */
    {0xF0, 0x90, 0x90, 0x90, 0xF0},
    /* 1 */
    {0x20, 0x60, 0x20, 0x20, 0x70},
    /* 2 */
    {0xF0, 0x10, 0xF0, 0x80, 0xF0},
    /* 3 */
    {0xF0, 0x10, 0xF0, 0x10, 0xF0},
    /* 4 */
    {0x90, 0x90, 0xF0, 0x10, 0x10},
    /* 5 */
    {0xF0, 0x80, 0xF0, 0x10, 0xF0},
    /* 6 */
    {0xF0, 0x80, 0xF0, 0x90, 0xF0},
    /* 7 */
    {0xF0, 0x10, 0x20, 0x40, 0x40},
    /* 8 */
    {0xF0, 0x90, 0xF0, 0x90, 0xF0},
    /* 9 */
    {0xF0, 0x90, 0xF0, 0x10, 0xF0},
    /* A */
    {0xF0, 0x90, 0xF0, 0x90, 0x90},
    /* B */
    {0xE0, 0x90, 0xE0, 0x90, 0xE0},
    /* C */
    {0xF0, 0x80, 0x80, 0x80, 0xF0},
    /* D */
    {0xE0, 0x90, 0x90, 0x90, 0xE0},
    /* E */
    {0xF0, 0x80, 0xF0, 0x80, 0xF0},
    /* F */
    {0xF0, 0x80, 0xF0, 0x80, 0x80}
};

/*
 * Initialize SDL app.
 */
bool sdl_app_init(void)
{
    SDL_SetAppMetadata("Chip-8 Emulator", "0.0.1", "com.example.emulator");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return false;
    }
    return true;
}

/*
 * Load default WAV file from assets.
 */
bool sdl_load_wav(AppState *as, SDL_AudioSpec *spec)
{
    char *wav_path;

    SDL_asprintf(&wav_path, "%sassets/441634__xtrgamr__asynth.wav",
                 SDL_GetBasePath());
    if (!SDL_LoadWAV(wav_path, spec, &as->wav_data, &as->wav_data_len)) {
        SDL_Log("Couldn't load .wav file: %s", SDL_GetError());
        SDL_free(wav_path);
        return false;
    }
    SDL_free(wav_path);
    return true;
}

/*
 * Create SDL open audio device stream.
 */
bool sdl_create_audio_stream(AppState *as, SDL_AudioSpec *spec)
{
    as->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                           spec, NULL, NULL);
    if (!as->stream) {
        SDL_Log("Couldn't create audio stream: %s", SDL_GetError());
        return false;
    }
    return true;
}

/*
 * Create SDL window and renderer.
 */
bool sdl_create_window_and_renderer(AppState *as)
{
    if (!SDL_CreateWindowAndRenderer("examples/emulator/chip-8",
                                     SDL_WINDOW_WIDTH, SDL_WINDOW_HEIGHT, 0,
                                     &as->window, &as->renderer)) {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        return false;
    }
    return true;
}

/*
 * Parse command line args.
 */
bool parse_command_line_args(int argc, char *argv[], AppState *appstate)
{
    int opt, fps, ipf;

    while ((opt = getopt(argc, argv, "dr:f:i:")) != -1) {
        switch (opt) {
        case 'd':
            appstate->enable_logs = true;
            break;
        case 'r':
            appstate->rom_file_path = optarg;
            break;
        case 'f':
            fps = atoi(optarg);
            if (fps >= 1 && fps <= 255) {
                appstate->fps = fps;
            } else {
                puts("FPS cannot be less than 1 or more than 255");
                return false;
            }
            break;
        case 'i':
            ipf = atoi(optarg);
            if (ipf >= 1 && ipf <= 255) {
                appstate->ipf = ipf;
            } else {
                puts("IPF cannot be less than 1 or more than 255");
                return false;
            }
            break;
        default: /* '?' */
            fprintf(stderr,
                    "Usage: %s [-d (enable instruction logs)] [-r "
                    "rom_file_path] [-f fps] [-i instructions_per_frame]\n",
                    argv[0]);
            return false;
        }
    }

    return true;
}

/*
 * Read passed on startup rom file path.
 */
bool read_rom_file(AppState *appstate)
{
    Uint32 rom_file_size;
    long   ftell_result;
    size_t read;
    FILE  *fp;

    if ((fp = fopen(appstate->rom_file_path, "rb")) == NULL) {
        fprintf(stderr, "ERROR: Could not read file %s: %s\n",
                appstate->rom_file_path, strerror(errno));
        return false;
    }

    /* Calculate file length */
    fseek(fp, 0, SEEK_END);
    ftell_result = ftell(fp);
    if (ftell_result < 0) {
        fprintf(stderr, "ERROR: Problem reading file\n");
        fclose(fp);
        return false;
    } else if (ftell_result > ROM_MAX_SIZE) {
        fprintf(stderr, "ERROR: Your rom is too big for CHIP-8: %s\n",
                appstate->rom_file_path);
        fclose(fp);
        return false;
    }
    rom_file_size = (Uint32)ftell_result;
    rewind(fp);

    read = fread(&appstate->chip8_context.RAM[0x200], 1, rom_file_size, fp);
    if (read != (size_t)rom_file_size) {
        fprintf(stderr, "ERROR: Partial ROM read\n");
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

/*
 * Load CHIP8 standard font into RAM.
 */
void load_font(AppState *appstate)
{
    Uint8 offset;

    offset = 0x0;
    for (size_t letter_number = 0; letter_number < FONT_SIZE; letter_number++) {
        for (size_t letter_byte_index = 0; letter_byte_index < FONT_BYTES;
             letter_byte_index++) {
            Uint8 letter_byte = DEFALT_FONT[letter_number][letter_byte_index];
            appstate->chip8_context.RAM[0x0 + offset++] = letter_byte;
        }
    }
}

/*
 * Initialize app, prepare AppState structure, keyboard buffer, audio stream.
 */
SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    SDL_AudioSpec spec;
    AppState     *as;

    as                   = (AppState *)SDL_calloc(1, sizeof(AppState));
    as->keyboard_state   = SDL_GetKeyboardState(NULL);
    as->chip8_context.PC = ROM_GAME_ADDRESS_START;
    as->ipf              = INSTRUCTIONS_PER_FRAME;
    as->fps              = FRAMES_PER_SECOND;
    as->need_redraw      = false;
    as->stop_execution   = false;
    *appstate            = as;

    memcpy(as->chip8_context.keyboard_keys, keys, sizeof(keys));
    SDL_zeroa(as->chip8_context.display_cells);
    srand((Uint32)time(NULL));

    if (!as || !sdl_app_init() || !sdl_load_wav(as, &spec) ||
        !sdl_create_audio_stream(as, &spec) ||
        !sdl_create_window_and_renderer(as) ||
        !parse_command_line_args(argc, argv, as) || !read_rom_file(as))
        return SDL_APP_FAILURE;

    as->nanoseconds_per_frame = NANOSECONDS_IN_SECOND / as->fps;
    load_font(as);

    return SDL_APP_CONTINUE;
}

/*
 * Handle new events: mouse input, keypresses, etc.
 */
SDL_AppResult SDL_AppEvent(SDL_UNUSED void *appstate, SDL_Event *event)
{
    switch (event->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_KEY_DOWN:
        if (event->key.scancode == SDL_SCANCODE_ESCAPE)
            return SDL_APP_SUCCESS;
    default:
        return SDL_APP_CONTINUE;
    }
}

Uint16 fetch_instruction(AppState *appstate)
{
    Uint16 instruction =
        ((appstate->chip8_context.RAM[appstate->chip8_context.PC] << 8) |
         appstate->chip8_context.RAM[appstate->chip8_context.PC + 1]);
    appstate->chip8_context.PC += 0x2;
    return instruction;
}

Uint8 get_display_cell(AppState *appstate, Sint16 row, Sint16 col)
{
    Uint32 shift = CHIP8_DISPLAY_WIDTH - col - 1;
    return (appstate->chip8_context.display_cells[row] >> shift & 0x1);
}

void stack_push_instruction(Uint16 instruction, AppState *appstate)
{
    if (appstate->chip8_context.SP >= 16) {
        exit(1);
    }
    appstate->chip8_context.stack[++appstate->chip8_context.SP] = instruction;
}

Uint16 stack_pop_instruction(AppState *appstate)
{
    if (appstate->chip8_context.SP > 0) {
        return appstate->chip8_context.stack[appstate->chip8_context.SP--];
    }
    return 0;
}

static void instruction_00E0(AppState *appstate, char **message)
{
    SDL_memset(appstate->chip8_context.display_cells, 0,
               sizeof(appstate->chip8_context.display_cells));
    appstate->need_redraw = true;
    if (appstate->enable_logs)
        snprintf(*message, 256, "Clear the display");
}

static void instruction_00EE(AppState *appstate, char **message)
{
    appstate->chip8_context.PC = stack_pop_instruction(appstate);
    if (appstate->enable_logs)
        snprintf(*message, 256, "Returning from a subroutine");
}

static void instruction_1nnn(AppState *appstate, char **message,
                             Uint16 second_third_and_fourth_nibbles)
{
    appstate->chip8_context.PC = second_third_and_fourth_nibbles;
    if (appstate->enable_logs)
        snprintf(*message, 256, "Jump to address %x",
                 second_third_and_fourth_nibbles);
}

static void instruction_2nnn(AppState *appstate, char **message,
                             Uint16 second_third_and_fourth_nibbles)
{
    stack_push_instruction(appstate->chip8_context.PC, appstate);
    appstate->chip8_context.PC = second_third_and_fourth_nibbles;
    if (appstate->enable_logs)
        snprintf(*message, 256, "Call the subroutine at memory location %x",
                 second_third_and_fourth_nibbles);
}

static void instruction_3xkk(AppState *appstate, char **message,
                             Uint8 second_nibble,
                             Uint8 third_and_fourth_nibbles)
{
    if (appstate->chip8_context.V[second_nibble] == third_and_fourth_nibbles) {
        appstate->chip8_context.PC += 0x2;
    }
    if (appstate->enable_logs)
        snprintf(*message, 256, "Skip next instruction if V%x is equal to %x",
                 second_nibble, third_and_fourth_nibbles);
}

static void instruction_4xkk(AppState *appstate, char **message,
                             Uint8 second_nibble,
                             Uint8 third_and_fourth_nibbles)
{
    if (appstate->chip8_context.V[second_nibble] != third_and_fourth_nibbles) {
        appstate->chip8_context.PC += 0x2;
    }
    if (appstate->enable_logs)
        snprintf(*message, 256,
                 "Skip next instruction if V%x is not equal to %x",
                 second_nibble, third_and_fourth_nibbles);
}

static void instruction_5xy0(AppState *appstate, char **message,
                             Uint8 second_nibble, Uint8 third_nibble)
{
    if (appstate->chip8_context.V[second_nibble] ==
        appstate->chip8_context.V[third_nibble]) {
        appstate->chip8_context.PC += 0x2;
    }
    if (appstate->enable_logs)
        snprintf(*message, 256, "Skip next instruction if V%x is equal to V%x",
                 second_nibble, third_nibble);
}

static void instruction_6xkk(AppState *appstate, char **message,
                             Uint8 second_nibble,
                             Uint8 third_and_fourth_nibbles)
{
    appstate->chip8_context.V[second_nibble] = third_and_fourth_nibbles;
    if (appstate->enable_logs)
        snprintf(*message, 256, "Set value %x to register V%x",
                 third_and_fourth_nibbles, second_nibble);
}

static void instruction_7xkk(AppState *appstate, char **message,
                             Uint8 second_nibble,
                             Uint8 third_and_fourth_nibbles)
{
    appstate->chip8_context.V[second_nibble] += third_and_fourth_nibbles;
    if (appstate->enable_logs)
        snprintf(*message, 256, "Add value %x to register V%x",
                 third_and_fourth_nibbles, second_nibble);
}

static void instruction_8xy0(AppState *appstate, char **message,
                             Uint8 second_nibble, Uint8 third_nibble)
{
    appstate->chip8_context.V[second_nibble] =
        appstate->chip8_context.V[third_nibble];
    if (appstate->enable_logs)
        snprintf(*message, 256, "V%x is set to the value of V%x", second_nibble,
                 third_nibble);
}

static void instruction_8xy1(AppState *appstate, char **message,
                             Uint8 second_nibble, Uint8 third_nibble)
{
    appstate->chip8_context.V[second_nibble] |=
        appstate->chip8_context.V[third_nibble];
    appstate->chip8_context.V[15] = 0;
    if (appstate->enable_logs)
        snprintf(*message, 256, "V%x is set to the bitwise OR of V%x and V%x",
                 second_nibble, second_nibble, third_nibble);
}

static void instruction_8xy2(AppState *appstate, char **message,
                             Uint8 second_nibble, Uint8 third_nibble)
{
    appstate->chip8_context.V[second_nibble] &=
        appstate->chip8_context.V[third_nibble];
    appstate->chip8_context.V[15] = 0;
    if (appstate->enable_logs)
        snprintf(*message, 256, "V%x is set to the bitwise AND of V%x and V%x",
                 second_nibble, second_nibble, third_nibble);
}

static void instruction_8xy3(AppState *appstate, char **message,
                             Uint8 second_nibble, Uint8 third_nibble)
{
    appstate->chip8_context.V[second_nibble] ^=
        appstate->chip8_context.V[third_nibble];
    appstate->chip8_context.V[15] = 0;
    if (appstate->enable_logs)
        snprintf(*message, 256, "V%x is set to the bitwise XOR of V%x and V%x",
                 second_nibble, second_nibble, third_nibble);
}

static void instruction_8xy4(AppState *appstate, char **message,
                             Uint8 second_nibble, Uint8 third_nibble)
{
    Uint16 add_result = appstate->chip8_context.V[second_nibble] +
                        appstate->chip8_context.V[third_nibble];
    appstate->chip8_context.V[second_nibble] = (Uint8)add_result;
    if (add_result > 255)
        appstate->chip8_context.V[15] = 1;
    else
        appstate->chip8_context.V[15] = 0;
    if (appstate->enable_logs)
        snprintf(*message, 256,
                 "V%x is set to the value of V%x plus the value of V%x",
                 second_nibble, second_nibble, third_nibble);
}

static void instruction_8xy5(AppState *appstate, char **message,
                             Uint8 second_nibble, Uint8 third_nibble)
{
    bool is_lower = appstate->chip8_context.V[second_nibble] >=
                    appstate->chip8_context.V[third_nibble];
    appstate->chip8_context.V[second_nibble] -=
        appstate->chip8_context.V[third_nibble];
    if (is_lower)
        appstate->chip8_context.V[15] = 1;
    else
        appstate->chip8_context.V[15] = 0;
    if (appstate->enable_logs)
        snprintf(*message, 256, "V%x is set to the value of V%x - V%x",
                 second_nibble, second_nibble, third_nibble);
}

static void instruction_8xy6(AppState *appstate, char **message,
                             Uint8 second_nibble, Uint8 third_nibble)
{
    Uint8 least_significant_bit;

    appstate->chip8_context.V[second_nibble] =
        appstate->chip8_context.V[third_nibble];
    if ((appstate->chip8_context.V[second_nibble] & 0x1) == 0x1) {
        least_significant_bit = 1;
    } else {
        least_significant_bit = 0;
    }
    appstate->chip8_context.V[second_nibble] >>= 1;
    appstate->chip8_context.V[15] = least_significant_bit;
    if (appstate->enable_logs)
        snprintf(*message, 256, "Right shift V%x", second_nibble);
}

static void instruction_8xy7(AppState *appstate, char **message,
                             Uint8 second_nibble, Uint8 third_nibble)
{
    bool is_lower = appstate->chip8_context.V[third_nibble] >=
                    appstate->chip8_context.V[second_nibble];
    appstate->chip8_context.V[second_nibble] =
        appstate->chip8_context.V[third_nibble] -
        appstate->chip8_context.V[second_nibble];
    if (is_lower)
        appstate->chip8_context.V[15] = 1;
    else
        appstate->chip8_context.V[15] = 0;
    if (appstate->enable_logs)
        snprintf(*message, 256,
                 "V%x is set to the value of V%x plus the value of V%x",
                 second_nibble, third_nibble, second_nibble);
}

static void instruction_8xyE(AppState *appstate, char **message,
                             Uint8 second_nibble, Uint8 third_nibble)
{
    Uint8 most_significant_bit;

    appstate->chip8_context.V[second_nibble] =
        appstate->chip8_context.V[third_nibble];
    if ((appstate->chip8_context.V[second_nibble] & 0x80) == 0x80) {
        most_significant_bit = 1;
    } else {
        most_significant_bit = 0;
    }
    appstate->chip8_context.V[second_nibble] <<= 1;
    appstate->chip8_context.V[15] = most_significant_bit;
    if (appstate->enable_logs)
        snprintf(*message, 256, "Left shift V%x", second_nibble);
}

static void instruction_9xy0(AppState *appstate, char **message,
                             Uint8 second_nibble, Uint8 third_nibble)
{
    if (appstate->chip8_context.V[second_nibble] !=
        appstate->chip8_context.V[third_nibble]) {
        appstate->chip8_context.PC += 0x2;
    }
    if (appstate->enable_logs)
        snprintf(*message, 256,
                 "Skip next instruction if V%x is not equal to V%x",
                 second_nibble, third_nibble);
}

static void instruction_Annn(AppState *appstate, char **message,
                             Uint16 second_third_and_fourth_nibbles)
{
    appstate->chip8_context.I = second_third_and_fourth_nibbles;
    if (appstate->enable_logs)
        snprintf(*message, 256, "Set address %x to register I",
                 second_third_and_fourth_nibbles);
}

static void instruction_Bnnn(AppState *appstate, char **message,
                             Uint8  second_nibble,
                             Uint16 second_third_and_fourth_nibbles)
{
    appstate->chip8_context.PC =
        second_third_and_fourth_nibbles + appstate->chip8_context.V[0];
    if (appstate->enable_logs)
        snprintf(*message, 256, "Jump to address %x + value from V%x",
                 second_third_and_fourth_nibbles, second_nibble);
}

static void instruction_Cxkk(AppState *appstate, char **message,
                             Uint8 second_nibble,
                             Uint8 third_and_fourth_nibbles)
{
    Uint8 random_number = (rand() & 0xFF) & third_and_fourth_nibbles;
    appstate->chip8_context.V[second_nibble] = random_number;
    if (appstate->enable_logs)
        snprintf(*message, 256, "Generate random number: %x", random_number);
}

static void instruction_dxyn(AppState *appstate, char **message,
                             Uint8 second_nibble, Uint8 third_nibble,
                             Uint8 fourth_nibble)
{
    Uint8   n, sprite_data, sprite_bit_value, bit_pos, display_row_bit;
    Uint16  x_coord, y_coord;
    Uint64 *display_row;

    x_coord = appstate->chip8_context.V[second_nibble] % CHIP8_DISPLAY_WIDTH;
    y_coord = appstate->chip8_context.V[third_nibble] % CHIP8_DISPLAY_HEIGHT;
    appstate->chip8_context.V[0xF] = 0;
    n                              = fourth_nibble;

    for (size_t y = 0; y < n; y++) {
        if (y_coord + y >= CHIP8_DISPLAY_HEIGHT)
            break;
        sprite_data =
            appstate->chip8_context.RAM[appstate->chip8_context.I + y];
        display_row = &appstate->chip8_context.display_cells[y_coord + y];
        for (short x = 0; x < 8; x++) {
            if (x_coord + x >= CHIP8_DISPLAY_WIDTH)
                break;
            sprite_bit_value = (sprite_data >> (7 - x)) & 0x1;
            bit_pos          = CHIP8_DISPLAY_WIDTH - 1 - (x_coord + x);
            display_row_bit  = (Uint8)(*display_row >> bit_pos) & 0x1;
            if (sprite_bit_value == 1 && display_row_bit == 1)
                appstate->chip8_context.V[0xF] = 1;
            *display_row ^= ((Uint64)sprite_bit_value << bit_pos);
        }
    }
    appstate->need_redraw = true;
    if (appstate->enable_logs)
        snprintf(
            *message, 256,
            "Display %x-byte sprite starting at memory location I at (V%x, "
            "V%x)",
            fourth_nibble, second_nibble, third_nibble);
}

static void instruction_Ex9E(AppState *appstate, char **message,
                             Uint8 second_nibble)
{
    Uint8 second_nibble_value = appstate->chip8_context.V[second_nibble];
    Uint8 key_scancode =
        appstate->chip8_context.keyboard_keys[second_nibble_value];
    if (appstate->keyboard_state[key_scancode]) {
        appstate->chip8_context.PC += 2;
        if (appstate->enable_logs)
            snprintf(*message, 256,
                     "Key %zx is pressed, next instruction skipped.",
                     (size_t)key_scancode);
    }
}

static void instruction_ExA1(AppState *appstate, char **message,
                             Uint8 second_nibble)
{
    Uint8 second_nibble_value = appstate->chip8_context.V[second_nibble];
    Uint8 key_scancode =
        appstate->chip8_context.keyboard_keys[second_nibble_value];
    if (!appstate->keyboard_state[key_scancode]) {
        appstate->chip8_context.PC += 2;
        if (appstate->enable_logs)
            snprintf(*message, 256,
                     "Key %zx is NOT pressed, next instruction skipped.",
                     (size_t)key_scancode);
    }
}

static void instruction_Fx07(AppState *appstate, char **message,
                             Uint8 second_nibble)
{
    appstate->chip8_context.V[second_nibble] =
        appstate->chip8_context.delay_timer;
    if (appstate->enable_logs)
        snprintf(*message, 256, "Set delay timer to V%x", second_nibble);
}

static void instruction_Fx0A(AppState *appstate, char **message,
                             Uint8 second_nibble)
{
    bool is_key_pressed = false;
    for (size_t key_index = 0; key_index < 16; key_index++) {
        Uint8 cur_scancode = appstate->chip8_context.keyboard_keys[key_index];
        if (appstate->keyboard_state[cur_scancode]) {
            snprintf(*message, 256, "Key %zx is pressed, number putted in V%x.",
                     key_index, second_nibble);
            SDL_ResetKeyboard();
            appstate->chip8_context.V[second_nibble] = (Uint8)key_index;
            is_key_pressed                           = true;
            break;
        }
    }
    if (!is_key_pressed) {
        appstate->chip8_context.PC -= 2;
        if (appstate->enable_logs)
            snprintf(*message, 256,
                     "Wait for key input, put key number into V%x",
                     second_nibble);
    }
}

static void instruction_Fx15(AppState *appstate, char **message,
                             Uint8 second_nibble)
{
    appstate->chip8_context.delay_timer =
        appstate->chip8_context.V[second_nibble];
    if (appstate->enable_logs)
        snprintf(*message, 256, "Set V%x to delay timer", second_nibble);
}

static void instruction_Fx18(AppState *appstate, char **message,
                             Uint8 second_nibble)
{
    appstate->chip8_context.sound_timer =
        appstate->chip8_context.V[second_nibble];
    if (appstate->enable_logs)
        snprintf(*message, 256, "Set V%x to sound timer", second_nibble);
}

static void instruction_Fx1E(AppState *appstate, char **message,
                             Uint8 second_nibble)
{
    appstate->chip8_context.I += appstate->chip8_context.V[second_nibble];
    if (appstate->enable_logs)
        snprintf(*message, 256, "Set I = I + V%x", second_nibble);
}

static void instruction_Fx29(AppState *appstate, char **message,
                             Uint8 second_nibble)
{
    appstate->chip8_context.I = 5 * appstate->chip8_context.V[second_nibble];
    if (appstate->enable_logs)
        snprintf(*message, 256, "Point I to the %x character", second_nibble);
}

static void instruction_Fx33(AppState *appstate, char **message,
                             Uint8 second_nibble)
{
    /* We need to split VX number to three decimal digits */
    Uint8 v_number = appstate->chip8_context.V[second_nibble];
    Uint8 number;
    Uint8 decimal_base = 10;
    for (int offset = 2; offset >= 0; offset--) {
        number = v_number % decimal_base;
        v_number /= decimal_base;
        appstate->chip8_context.RAM[appstate->chip8_context.I + offset] =
            number;
    }
    if (appstate->enable_logs)
        snprintf(*message, 256,
                 "Store BCD representation of V%x in memory locations I, I+1, "
                 "and I+2",
                 second_nibble);
}

static void instruction_Fx55(AppState *appstate, char **message,
                             Uint8 second_nibble)
{
    if (second_nibble == 0) {
        appstate->chip8_context.RAM[appstate->chip8_context.I++] =
            appstate->chip8_context.V[0];
    } else {
        for (int register_number = 0; register_number <= second_nibble;
             register_number++) {
            appstate->chip8_context.RAM[appstate->chip8_context.I++] =
                appstate->chip8_context.V[register_number];
        }
    }
    if (appstate->enable_logs)
        snprintf(
            *message, 256,
            "Store registers V0 through V%x in memory starting at location "
            "I",
            second_nibble);
}

static void instruction_Fx65(AppState *appstate, char **message,
                             Uint8 second_nibble)
{
    if (second_nibble == 0) {
        appstate->chip8_context.V[0] =
            appstate->chip8_context.RAM[appstate->chip8_context.I++];
    } else {
        for (int register_number = 0; register_number <= second_nibble;
             register_number++) {
            appstate->chip8_context.V[register_number] =
                appstate->chip8_context.RAM[appstate->chip8_context.I++];
        }
    }
    if (appstate->enable_logs)
        snprintf(*message, 256,
                 "Read registers V0 through V%x from memory starting at "
                 "location I",
                 second_nibble);
}

void decode_instruction(AppState *appstate, char **message, Uint16 instruction)
{
    /* Get first nibble (to decode) from instruction using `bit mask` and `and`
     */
    Uint8  first_nibble             = GET_FIRST_NIBBLE(instruction);
    Uint8  second_nibble            = GET_SECOND_NIBBLE(instruction);
    Uint8  third_nibble             = GET_THIRD_NIBBLE(instruction);
    Uint8  fourth_nibble            = GET_FOURTH_NIBBLE(instruction);
    Uint8  third_and_fourth_nibbles = GET_THIRD_AND_FORTH_NIBBLES(instruction);
    Uint16 second_third_and_fourth_nibbles =
        GET_SECOND_THIRD_AND_FOURTH_NIBBLES(instruction);

    switch (first_nibble) {
    case 0x0:
        switch (fourth_nibble) {
        case 0x0:
            instruction_00E0(appstate, message);
            break;
        case 0xE:
            instruction_00EE(appstate, message);
            break;
        default:
            snprintf(*message, 256, "Not an original Chip-8 instruction");
            appstate->stop_execution = true;
            break;
        }
        break;
    case 0x1:
        instruction_1nnn(appstate, message, second_third_and_fourth_nibbles);
        break;
    case 0x2:
        instruction_2nnn(appstate, message, second_third_and_fourth_nibbles);
        break;
    case 0x3:
        instruction_3xkk(appstate, message, second_nibble,
                         third_and_fourth_nibbles);
        break;
    case 0x4:
        instruction_4xkk(appstate, message, second_nibble,
                         third_and_fourth_nibbles);
        break;
    case 0x5:
        instruction_5xy0(appstate, message, second_nibble, third_nibble);
        break;
    case 0x6:
        instruction_6xkk(appstate, message, second_nibble,
                         third_and_fourth_nibbles);
        break;
    case 0x7:
        instruction_7xkk(appstate, message, second_nibble,
                         third_and_fourth_nibbles);
        break;
    case 0x8:
        switch (fourth_nibble) {
        case 0x0:
            instruction_8xy0(appstate, message, second_nibble, third_nibble);
            break;
        case 0x1:
            instruction_8xy1(appstate, message, second_nibble, third_nibble);
            break;
        case 0x2:
            instruction_8xy2(appstate, message, second_nibble, third_nibble);
            break;
        case 0x3:
            instruction_8xy3(appstate, message, second_nibble, third_nibble);
            break;
        case 0x4:
            instruction_8xy4(appstate, message, second_nibble, third_nibble);
            break;
        case 0x5:
            instruction_8xy5(appstate, message, second_nibble, third_nibble);
            break;
        case 0x6:
            instruction_8xy6(appstate, message, second_nibble, third_nibble);
            break;
        case 0x7:
            instruction_8xy7(appstate, message, second_nibble, third_nibble);
            break;
        case 0xE:
            instruction_8xyE(appstate, message, second_nibble, third_nibble);
            break;
        default:
            snprintf(*message, 256, "Not an original Chip-8 instruction");
            appstate->stop_execution = true;
            break;
        }
        break;
    case 0x9:
        instruction_9xy0(appstate, message, second_nibble, third_nibble);
        break;
    case 0xA:
        instruction_Annn(appstate, message, second_third_and_fourth_nibbles);
        break;
    case 0xB:
        instruction_Bnnn(appstate, message, second_nibble,
                         second_third_and_fourth_nibbles);
        break;
    case 0xC:
        instruction_Cxkk(appstate, message, second_nibble,
                         third_and_fourth_nibbles);
        break;
    case 0xD:
        instruction_dxyn(appstate, message, second_nibble, third_nibble,
                         fourth_nibble);
        break;
    case 0xE:
        switch (third_and_fourth_nibbles) {
        case 0x9E:
            instruction_Ex9E(appstate, message, second_nibble);
            break;
        case 0xA1:
            instruction_ExA1(appstate, message, second_nibble);
            break;
        default:
            snprintf(*message, 256, "Not an original Chip-8 instruction");
            appstate->stop_execution = true;
            break;
        }
        break;
    case 0xF:
        switch (third_and_fourth_nibbles) {
        case 0x07:
            instruction_Fx07(appstate, message, second_nibble);
            break;
        case 0x0A:
            instruction_Fx0A(appstate, message, second_nibble);
            break;
        case 0x15:
            instruction_Fx15(appstate, message, second_nibble);
            break;
        case 0x18:
            instruction_Fx18(appstate, message, second_nibble);
            break;
        case 0x1E:
            instruction_Fx1E(appstate, message, second_nibble);
            break;
        case 0x29:
            instruction_Fx29(appstate, message, second_nibble);
            break;
        case 0x33:
            instruction_Fx33(appstate, message, second_nibble);
            break;
        case 0x55:
            instruction_Fx55(appstate, message, second_nibble);
            break;
        case 0x65:
            instruction_Fx65(appstate, message, second_nibble);
            break;
        default:
            snprintf(*message, 256, "Not an original Chip-8 instruction");
            appstate->stop_execution = true;
            break;
        }
        break;
    default:
        snprintf(*message, 256, "Not an original Chip-8 instruction");
        appstate->stop_execution = true;
        break;
    }
}

static void set_rect_xy_(SDL_FRect *r, Uint16 x, Uint16 y)
{
    r->x = (float)(x * PIXEL_SIZE);
    r->y = (float)(y * PIXEL_SIZE);
}

void draw_screen(AppState *appstate)
{
    Uint8     display_cell;
    SDL_FRect r;
    Uint16    i;
    Uint16    j;

    r.w = r.h = PIXEL_SIZE;

    SDL_SetRenderDrawColor(appstate->renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(appstate->renderer);

    for (j = 0; j < CHIP8_DISPLAY_HEIGHT; j++) {
        for (i = 0; i < CHIP8_DISPLAY_WIDTH; i++) {
            display_cell = get_display_cell(appstate, j, i);
            if (display_cell == 0) {
                SDL_SetRenderDrawColor(appstate->renderer, 0, 0, 0,
                                       SDL_ALPHA_OPAQUE);
            } else {
                SDL_SetRenderDrawColor(appstate->renderer, 255, 255, 255,
                                       SDL_ALPHA_OPAQUE);
            }
            set_rect_xy_(&r, i, j);
            SDL_RenderFillRect(appstate->renderer, &r);
        }
    }

    SDL_RenderPresent(appstate->renderer);
}

/*
 * Process single iteration of program's main loop.
 */
SDL_AppResult SDL_AppIterate(void *appstate)
{
    Uint64    start_ticks, end_ticks;
    char      message[256];
    char     *message_ptr;
    Uint64    elapsed_ns;
    AppState *as;

    as          = (AppState *)appstate;
    message_ptr = message;

    start_ticks = SDL_GetTicksNS();

    if (as->chip8_context.delay_timer > 0)
        --as->chip8_context.delay_timer;
    if (as->chip8_context.sound_timer > 0) {
        SDL_ResumeAudioStreamDevice(as->stream);
        if (SDL_GetAudioStreamQueued(as->stream) < (int)as->wav_data_len) {
            SDL_PutAudioStreamData(as->stream, as->wav_data, as->wav_data_len);
        }
        --as->chip8_context.sound_timer;
    } else {
        SDL_PauseAudioStreamDevice(as->stream);
    }

    if (as->need_redraw && !as->chip8_context.vblank_sync) {
        draw_screen(as);
        as->need_redraw = false;
    }
    as->chip8_context.vblank_sync = false;
    for (size_t instructions_count = 1; instructions_count <= as->ipf;
         instructions_count++) {
        Uint16 cur_instruction = fetch_instruction(as);
        decode_instruction(as, &message_ptr, cur_instruction);
        if (as->enable_logs) {
            printf("%04x: %s\n", cur_instruction, message);
        }
        if (as->need_redraw && !as->chip8_context.vblank_sync) {
            draw_screen(as);
            as->need_redraw               = false;
            as->chip8_context.vblank_sync = true;
        } else if (as->need_redraw && as->chip8_context.vblank_sync) {
            break;
        }
        if (as->stop_execution) {
            return SDL_APP_FAILURE;
        }
    }

    end_ticks  = SDL_GetTicksNS();
    elapsed_ns = end_ticks - start_ticks;
    if (elapsed_ns < as->nanoseconds_per_frame) {
        SDL_DelayNS(as->nanoseconds_per_frame - elapsed_ns);
    }

    return SDL_APP_CONTINUE;
}

/*
 * Destroy renderer, window, clean allocations on exit.
 */
void SDL_AppQuit(void *appstate, SDL_UNUSED SDL_AppResult result)
{
    if (appstate != NULL) {
        AppState *as = (AppState *)appstate;
        SDL_DestroyRenderer(as->renderer);
        SDL_DestroyWindow(as->window);
        SDL_free(as->wav_data);
        SDL_free(as);
    }
}
