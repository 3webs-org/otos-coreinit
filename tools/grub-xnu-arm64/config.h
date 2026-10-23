/* Minimal config.h for building the xnu loader module out of tree.
 *
 * A configured GRUB tree has a generated config.h; an unconfigured
 * source tree does not, and include/grub/types.h includes it
 * unconditionally. These are the only two symbols the loader path
 * actually consults -- GRUB_DEFAULT_HZ for its timer conversions, and
 * HAVE_CPU_SOFTIRQ for a softirq path the loader does not take.
 * Everything else the generated header carries is host- and
 * architecture-selection state that a loadable module never needs,
 * because the running GRUB kernel already made those choices.
 *
 * If you build against a configured tree, its config.h wins: pass
 * GRUB_SRC as a configured tree and this directory is searched second. */
#define GRUB_DEFAULT_HZ 100
#define HAVE_CPU_SOFTIRQ 0
