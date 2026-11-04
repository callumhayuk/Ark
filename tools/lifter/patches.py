"""Per-instruction replacements applied by gen.py: guest address -> function(gen, addr) returning C code.

Each checks the instruction it replaces (Legends: Arceus 1.1.1) and leaves any other word as it was.
"""


def _orig(gen, a):
    return ' '.join(gen.render_single(gen.lift(a), a, lambda t: f'goto L_{t:x};', set()))


def terrain_range(gen, a):
    # The terrain's level-of-detail test (main+0x2471048): a patch is split into four finer ones when
    # distance - half its size < [manager+520] (40 m, set in the manager's constructor). This load of that limit also
    # multiplies it by the run-time factor (ark_terrain_range, runtime/patches.c: ARK_TERRAIN_RANGE, F7).
    if gen.word(a) != 0xbd420905:  # ldr s5, [x8, #520]
        return _orig(gen, a)
    return ('{ uint64_t ea = c->x[8] + 0x208ull; c->v[5] = LD32(ea); '
            'extern void ark_terrain_range(Cpu *); ark_terrain_range(c); }')


def shiny_mode(gen, a):
    # pml's Pokémon creation (main+0x2B8BAC0) reads the spec's PID mode (spec+24): 0x3_FFFFFFFF = random PID with
    # shiny rolls, 0x1_FFFFFFFF = random but never shiny. With the shiny multiplier at 0 (ark_shiny_mode,
    # runtime/patches.c) the first becomes the second.
    if gen.word(a) != 0xf9400e68:  # ldr x8, [x19, #24]
        return _orig(gen, a)
    return ('{ extern uint64_t ark_shiny_mode(uint64_t); '
            'c->x[8] = ark_shiny_mode(LD64(c->x[19] + 0x18ull)); }')


def shiny_rolls(gen, a):
    # ... and in that mode draws up to [spec+57] PIDs, stopping at the first shiny one (1 in 4096 each). This load of
    # the roll count also multiplies it (ark_shiny_rolls: the menu's shiny slider, ark.ini shiny_rolls).
    if gen.word(a) != 0x3940e668:  # ldrb w8, [x19, #57]
        return _orig(gen, a)
    return ('{ extern uint32_t ark_shiny_rolls(uint32_t); '
            'c->x[8] = ark_shiny_rolls((uint32_t)LD8(c->x[19] + 0x39ull)); }')


def alpha_pick(gen, a):
    # The spawner's call of its weighted slot pick (main+0xF95DA0; the slot's field 46 marks alpha slots, see
    # ark_alpha_pick in runtime/patches.c) goes through ark_alpha_pick, which picks again up to N times until it
    # lands on an alpha slot (the menu's alpha slider, ark.ini alpha_rolls).
    if gen.word(a) != 0x9400063a:  # bl main+0xF95DA0
        return _orig(gen, a)
    t = a + 0x63a * 4
    return (f'{{ extern void ark_alpha_pick(Cpu *, void (*)(Cpu *)); extern void f_{t:x}(Cpu *); '
            f'c->x[30] = {a + 4:#x}ull; ark_alpha_pick(c, f_{t:x}); }}')


PATCHES = {
    0x70024713c4: terrain_range,
    0x7002b8bbfc: shiny_mode,
    0x7002b8bc28: shiny_rolls,
    0x7000f944b8: alpha_pick,
}
