/* C library headers must use the implementation-reserved namespace for
 * guards, type plumbing, and implementation extensions so they cannot
 * collide with users.
 */
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

/* SPDX-FileCopyrightText: (C) 2026 Gavin John
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _OWNERSHIP_H
#define _OWNERSHIP_H

/* An opt-in ownership dialect expressed entirely in ordinary C syntax.
 * Token policy belongs to the nominal token typedef; values and operations
 * refer to that policy by name. */
#define __token_type typedef struct { char _tok; }
#define tokdef __token_type
/* Every qualifier/relation macro below exists purely for the benefit of
 * the tools/clang checkers' AST walks (OwnershipChecker, MemoryContractChecker,
 * AllocationLifetimeChecker, and SizeCastChecker's ntlibc.ArrayIndex): none
 * of them is read by any normal compile. GCC parses annotate() but always
 * reports it "ignored" outside its own LTO-streaming consumer, and even
 * clang's own ordinary compiles never look at it -- only clang's static
 * analyzer engine (tools/lint.sh's --analyze-based stages) and the one
 * -fsyntax-only "totality" plugin stage that also needs withtok(...) for
 * null_terminated consult it. __clang_analyzer__ is clang's own predefined
 * macro for exactly the first case, defined whenever "clang --analyze" is
 * running regardless of which -analyzer-checker=... is loaded (the same
 * convention src/mman/mman.c's unsafe_pointer.h and
 * src/thread/pthread_cond.c's lock_requires_held_on_entry()/
 * lock_acquires_for_caller already use for their own, unrelated
 * annotations). tools/lint.sh's stage_totality builds its plugin as a
 * plain PluginASTAction under -fsyntax-only, which never defines
 * __clang_analyzer__, so that one stage also defines
 * NTLIBC_OWNERSHIP_ANALYSIS explicitly on its real-source scan; every
 * other stage that reads one of these annotations always runs under
 * --analyze and needs no extra flag. Emitting the attribute for a plain
 * GCC/clang build (including the "lint (warn)" -Wall -Wextra gate, and
 * tcc, which does not understand __attribute__((annotate(...))) at all)
 * serves no purpose there and is exactly what GCC's own "annotate"
 * warning is complaining about. */
#if defined(__clang_analyzer__) || defined(NTLIBC_OWNERSHIP_ANALYSIS)
#define __ownership_attr(text) __attribute__((annotate(text)))
#else
#define __ownership_attr(text)
#endif
/* Linear tokens are strict by default.  A permissive token remains linear,
 * but may stay behind while other, unlimited tokens on its carrier copy. */
#define l_strict __ownership_attr("qual:l_strict")
#define l_permissive __ownership_attr("qual:l_permissive")
#define l_unlimited __ownership_attr("qual:l_unlimited")
#define implicit_drop __ownership_attr("qual:implicit_drop")
#define dynamic_storage __ownership_attr("qual:dynamic_storage")
#define implemented_by(token_name) \
	__ownership_attr("qual:implemented_by=" #token_name)
#define string_literal __ownership_attr("qual:string_literal")
#define extent_at_least __ownership_attr("qual:extent_at_least")
#define element_extent __ownership_attr("qual:element_extent")
#define disjoint_extent __ownership_attr("qual:disjoint_extent")
#define zero_vacuous __ownership_attr("qual:zero_vacuous")
#define sentinel_exclude(value) \
	__ownership_attr("qual:sentinel_exclude=" #value)
#define blocks_dereference \
	__ownership_attr("qual:blocks_dereference")
/* Two parametric siblings of sentinel_exclude(value) above, for a PLAIN
 * scalar (int/long) return value or parameter instead of a tokdef'd opaque
 * handle. sentinel_exclude names the sentinel a whole nominal token FAMILY
 * excludes (nl_catd's (nl_catd)-1, iconv_t's (iconv_t)-1, locale_t's
 * (locale_t)0); integer_sentinel/long_sentinel instead name the sentinel
 * ONE function's return, or ONE parameter, excludes -- the shape
 * src/util/timeout.c's parse_duration() needed (a plain `long` return whose
 * -1 means "unrepresentable duration", proven unchecked before a time_t
 * cast only by hand, in commit 1c4fc3b2).
 *
 * Attach it exactly where fallible above already attaches, immediately
 * before the declaration it describes:
 *
 *   integer_sentinel(-1)
 *   long parse_duration(const char *text);
 *
 * or on one parameter, the same way withtok(...) attaches:
 *
 *   int consume(long value integer_sentinel(-1));
 *
 * tools/clang/TokenAlgebra.h's scalarSentinel() reads either spelling back
 * off the Decl it was written on (a FunctionDecl for a return value, a
 * ParmVarDecl for a parameter) using the exact same literal grammar
 * sentinel_exclude(value) already parses (a bare NULL, or a base-10 integer
 * that fits int64_t) -- see sentinelFromQualifier's own comment for why
 * that parser is shared rather than reimplemented here.
 * tools/clang/SizeCastChecker.cpp's ntlibc.IntegerSentinel checker requires
 * every arithmetic (+ - * /), explicit-cast, or array/pointer-index use of
 * a value carrying one of these to be on a path that has already proven
 * the value is not that literal -- the same path-sensitive proof
 * TokenAlgebra.h's splitOnExcludedSentinel already performs for
 * sentinel_exclude, reused as-is rather than re-derived.
 *
 * integer_sentinel and long_sentinel are byte-for-byte the same mechanism
 * under two names, so a reader can pick the name matching the annotated
 * value's own declared width the same way readable_span/readable_elements
 * in include/memory_tokens.h document byte vs. element units even though
 * both compile to the same extent_at_least/element_extent machinery.
 * There is deliberately no floating-point sibling yet: the proof goes
 * through SValBuilder::makeIntVal, which only builds integer and pointer
 * constants, and IEEE-754 equality has real semantics (NaN, signed zero)
 * a literal integer comparison does not model -- a double_sentinel would
 * need its own, separately-reasoned proof, not this one reused as-is. */
#define integer_sentinel(value) \
	__ownership_attr("qual:integer_sentinel=" #value)
#define long_sentinel(value) \
	__ownership_attr("qual:long_sentinel=" #value)
/* The two thread-scoped verbs below attach a token operation to a bare
 * function declaration that has no natural parameter or return value to
 * hang a per-value withtok(...)/consume(...)/grant(...)/drop(...)
 * annotation on (e.g. close(), __errno_location()): the fact they track is
 * "one thing per analyzed path/function", not tied to any specific value.
 * There is no concurrency modeling anywhere in this checker suite --
 * "thread-scoped" names the granularity (once per analysis, like a
 * thread-local), not an actual thread.
 *
 * There is no consumes_thread_token/requires_thread_token_absent pair
 * alongside these: the one real design that motivated them (errno_pending,
 * tracking whether the single most-recently-diagnosed capable call is
 * still the most recent one at all) turned out to need per-call identity a
 * family-only key structurally cannot carry -- see
 * tools/clang/ErrnoDisciplineChecker.cpp's own design note on
 * CarrierCapabilityKind/ThreadCapabilityMap for the adversarial case that
 * sank it. Add that pair once a real, sound consumer needs it. */
#define grants_thread_token(token_name) \
	__ownership_attr("grants_thread_token:" #token_name)
#define requires_thread_token(token_name) \
	__ownership_attr("requires_thread_token:" #token_name)
/* The negative-space counterparts of the two verbs above. A prior
 * attempt at requires_thread_token_absent specifically for errno_pending
 * (tracking whether the single most-recently-diagnosed capable call is
 * still the most recent one at all) needed per-call identity a
 * family-only key structurally cannot carry -- see
 * tools/clang/ErrnoDisciplineChecker.cpp's own design note on
 * CarrierCapabilityKind/ThreadCapabilityMap for that adversarial case.
 * That specific use case is still unsound and remains unimplemented for
 * it. These two verbs are sound for the general case, though: any fact
 * that is purely "is this currently held on this path at all" (not "was
 * it *this particular* call that most recently established it") has no
 * such adversarial case, because there is no competing identity to
 * confuse it with -- ntlibc.ErrnoDiscipline's own generic thread-token
 * pass (distinct from its errno_grounds-specific one) uses exactly this
 * for arbitrary caller-supplied families. */
#define drops_thread_token(token_name) \
	__ownership_attr("drops_thread_token:" #token_name)
#define requires_thread_token_absent(token_name) \
	__ownership_attr("requires_thread_token_absent:" #token_name)
#define withtok(token_name) \
	__ownership_attr("withtok:" #token_name)
#define elements_withtok(token_name, extent_name) \
	__ownership_attr("elements_withtok:" #token_name ":" #extent_name)
#define withhandle(handle_name) \
	__ownership_attr("withhandle:" #handle_name)
#define withouttok(token_name) \
	__ownership_attr("withouttok:" #token_name)
#define consume(token_name) \
	__ownership_attr("consume:" #token_name)
#define consume_any(token_name) \
	__ownership_attr("consume_any:" #token_name)
#define grant(token_name) \
	__ownership_attr("grant:" #token_name)
#define drop(token_name) \
	__ownership_attr("drop:" #token_name)
#define consume_if_nonnull_return(token_name) \
	__ownership_attr("consume_if_nonnull_return:" #token_name)
#define construct(handle_name) \
	__ownership_attr("construct:" #handle_name)
#define destroy(handle_name) \
	__ownership_attr("destroy:" #handle_name)
#define handle(handle_name) \
	__ownership_attr("handle:" #handle_name)
#define static_handle(handle_name) \
	__ownership_attr("static_handle:" #handle_name)
/* A pointer-to-struct parameter whose pointee type carries its own
 * withtok(readable_elements(...))/withtok(writable_elements(...)) field
 * contracts (see include/memory_tokens.h). fields_established is a real,
 * two-sided obligation, not a blind trust of whatever the fields happen
 * to hold: MemoryContractChecker's checkPreCall independently verifies,
 * from the CALLER's own current knowledge, that every one of those field
 * contracts already holds for the argument BEFORE allowing the call --
 * exactly the same "prove it at the call site" discipline withtok(...)
 * Require parameters already get. Only once that is satisfied does
 * checkBeginFunction seed the callee's own reasoning (so a function
 * analyzed on its own, with no visible caller, is not forced to treat an
 * incoming struct's fields as permanently unprovable). Omitting this on
 * a parameter that is actually mutated is always safe -- the checker
 * just treats the incoming fields as unconstrained, as it always did. */
#define fields_established \
	__ownership_attr("fields_established")
/* A bare, function-level marker: this function's return value is a real
 * result, not an advisory one, and must not be silently discarded.
 * tools/clang/FallibleResultChecker.cpp's ntlibc.FallibleResult reads this
 * back off the callee instead of keeping its own hardcoded name list. */
#define fallible \
	__ownership_attr("fallible")
/* A bare qualifier on a tokdef: this token names a lock actually held (a
 * mutex/rwlock/spinlock's locked, shared, or exclusive state), as opposed
 * to that same lock's unlocked state. tools/clang/LockAlgebra.h reads
 * this back off a consume:/consume_any:/grant:/withtok: token name to
 * classify a pthread.h lock call's real protocol, instead of matching the
 * literal substrings _locked/_shared/_exclusive against the token's own
 * spelling. */
#define lock_held \
	__ownership_attr("qual:lock_held")
/* Two more bare, function-level markers, both read off the callee's
 * redecls the same way fallible is. They are related but genuinely
 * distinct real-world facts -- not two spellings of the same thing -- so
 * a function can carry either, both, or neither:
 *   - async_signal_safe: this function is on POSIX's own Async-Signal-Safe
 *     Functions table, so it may be called from within a signal handler.
 *     tools/clang/SignalSafetyChecker.cpp's asyncSafe() reads this back
 *     instead of keeping its own hardcoded copy of that POSIX table.
 *     Several members of that table (sigemptyset(), getpid(), umask(),
 *     kill(), sleep(), ...) do no I/O at all, and conversely several real
 *     I/O calls (mmap(), ioctl(), pread(), ...) are NOT on the table
 *     (their libc implementations may take an internal lock), so this is
 *     not implied by, and does not imply, io_operation below.
 *   - io_operation: this function performs I/O or a syscall, which
 *     tools/clang/PurityChecker.cpp's isIoCall() treats as a real,
 *     externally observable side effect that disqualifies
 *     __attribute__((pure)) eligibility. Reads this back instead of
 *     keeping its own hardcoded I/O name list.
 * A function commonly carries both (e.g. read(), write(), open()) since
 * most async-signal-safe calls are themselves I/O, but neither table is a
 * subset of the other -- see the two bullets above. */
#define async_signal_safe \
	__ownership_attr("async_signal_safe")
#define io_operation \
	__ownership_attr("io_operation")
/* A parameter-level marker for the strtol()/strtoul()/strtod()/...
 * "endptr" shape: a T** at some parameter index K whose pointee, WHENEVER
 * this function returns, the C standard requires to hold a pointer at or
 * beyond wherever parameter 0's own T* pointed on entry -- never behind
 * it, even on a completely failed conversion (then *endptr == parameter
 * 0's own value, unchanged). This is a real, load-bearing contract
 * (C11 7.22.1.1p5 and its strtoul/strtod/... siblings, all specified in
 * the same terms), asserted here by hand exactly the way
 * withtok(null_terminated) already asserts strlen()'s contract in
 * string.h -- not independently reverified against these functions'
 * bodies, which this project does not implement and Nature (or the host
 * libc, if this codebase's own ntlibc doesn't provide it either) does.
 *
 * TotalityChecker.cpp's own recursive-descent-parser proof reads this
 * back (see toleratedPointerReassign()) to recognize the extremely
 * common `T *end; v = strtol(cursor, &end, base); cursor = end;` idiom
 * as never moving a tracked parser cursor field BACKWARD -- which is all
 * that idiom needs to be safe to fold into an already-established
 * forward-progress proof from a witness elsewhere in the same
 * recursive-descent step. It is deliberately not itself treated as a
 * witness (a strict decrease): parameter 0 and *endptr are equal on a
 * totally failed conversion, so by itself this call proves only "did not
 * go backward", the same non-strict half of the proof a plain `x = x;`
 * would contribute. Nothing else in this project's checkers reads this
 * annotation. */
#define endptr_advances \
	__ownership_attr("qual:endptr_advances")

#endif

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
