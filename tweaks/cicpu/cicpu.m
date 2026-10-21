/*
 * cicpu.m - make CoreImage fail cleanly on ios-sweet (no GPU, no Metal device).
 *
 * Problem: -[CIContext initWithOptions:] checks _CIGLIsUsable(), which is true
 * on SWEET, then creates CI::GLContext. The GL function table is empty, so
 * CI::GLContext::GLContext jumps to pc 0 (SIGSEGV). Every CoreImage client
 * (MobileIcons icon compositing, Photos, ...) hits this.
 *
 * Fix: replace the CIContext designated initializers with versions that return
 * nil when no GPU is present (the default on this device; CICPU_GPU=1 keeps the
 * original behaviour). Callers then get a nil context and a NULL CGImage, and
 * the original code path is never entered. Only processes that would have
 * crashed in CoreImage behave differently.
 *
 * Installed from a constructor. If CoreImage is not loaded yet (DYLD_INSERT
 * runs before some frameworks), an add-image callback installs the hooks when
 * CIContext appears. The hooks are plain Objective-C runtime swizzles, so no
 * ElleKit or fishhook is needed. Build with tweaks/cicpu/Makefile (MRC).
 *
 * Env:
 *   CICPU_GPU=1      keep the original implementations (device with a GPU)
 *   CICPU_LOG=0      silence the one-line log per hooked selector
 */
#include <dlfcn.h>
#include <objc/NSObject.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef id (*init1_t)(id, SEL, id);
typedef id (*init2_t)(id, SEL, id, id);

static int tw_installed;
static int tw_log_enabled = 1;

static init1_t tw_orig_initWithOptions;
static init1_t tw_orig_initWithEAGLContext;
static init2_t tw_orig_initWithEAGLContextOptions;
static init2_t tw_orig_initWithMTLDeviceOptions;
static init2_t tw_orig_initWithMTLCommandQueueOptions;

static int
tw_gpu_enabled(void)
{
	const char *v = getenv("CICPU_GPU");
	return v != NULL && v[0] == '1';
}

static void
tw_log_once(SEL sel)
{
	static const char *seen[8];
	static int n;
	const char *name = sel_getName(sel);
	if (!tw_log_enabled) {
		return;
	}
	for (int i = 0; i < n; i++) {
		if (strcmp(seen[i], name) == 0) {
			return;
		}
	}
	if (n < 8) {
		seen[n++] = name;
	}
	fprintf(stderr, "cicpu: %s -> nil (no GPU on ios-sweet)\n", name);
}

/* Returning nil from an init must release self under MRC. */
static id
tw_initWithOptions(id self, SEL _cmd, id options)
{
	if (tw_gpu_enabled()) {
		return tw_orig_initWithOptions(self, _cmd, options);
	}
	tw_log_once(_cmd);
	[self release];
	return nil;
}

static id
tw_initWithEAGLContext(id self, SEL _cmd, id eagl)
{
	if (tw_gpu_enabled()) {
		return tw_orig_initWithEAGLContext(self, _cmd, eagl);
	}
	tw_log_once(_cmd);
	[self release];
	return nil;
}

static id
tw_initWithEAGLContextOptions(id self, SEL _cmd, id eagl, id options)
{
	if (tw_gpu_enabled()) {
		return tw_orig_initWithEAGLContextOptions(self, _cmd, eagl, options);
	}
	tw_log_once(_cmd);
	[self release];
	return nil;
}

static id
tw_initWithMTLDeviceOptions(id self, SEL _cmd, id device, id options)
{
	if (tw_gpu_enabled()) {
		return tw_orig_initWithMTLDeviceOptions(self, _cmd, device, options);
	}
	tw_log_once(_cmd);
	[self release];
	return nil;
}

static id
tw_initWithMTLCommandQueueOptions(id self, SEL _cmd, id queue, id options)
{
	if (tw_gpu_enabled()) {
		return tw_orig_initWithMTLCommandQueueOptions(self, _cmd, queue, options);
	}
	tw_log_once(_cmd);
	[self release];
	return nil;
}

/* Swap one instance method on CIContext; keep the original IMP for the gpu path. */
static IMP
tw_swap(Class cls, SEL sel, IMP replacement)
{
	Method m = class_getInstanceMethod(cls, sel);
	if (m == NULL) {
		return NULL;
	}
	return method_setImplementation(m, replacement);
}

static void
tw_install(void)
{
	if (tw_installed) {
		return;
	}
	Class ci = objc_getClass("CIContext");
	if (ci == Nil) {
		return;             /* CoreImage not loaded yet */
	}
	tw_installed = 1;
	if (getenv("CICPU_LOG") && getenv("CICPU_LOG")[0] == '0') {
		tw_log_enabled = 0;
	}
	tw_orig_initWithOptions = (init1_t)tw_swap(ci, sel_registerName("initWithOptions:"),
	    (IMP)tw_initWithOptions);
	tw_orig_initWithEAGLContext = (init1_t)tw_swap(ci, sel_registerName("initWithEAGLContext:"),
	    (IMP)tw_initWithEAGLContext);
	tw_orig_initWithEAGLContextOptions = (init2_t)tw_swap(ci,
	    sel_registerName("initWithEAGLContext:options:"), (IMP)tw_initWithEAGLContextOptions);
	tw_orig_initWithMTLDeviceOptions = (init2_t)tw_swap(ci,
	    sel_registerName("initWithMTLDevice:options:"), (IMP)tw_initWithMTLDeviceOptions);
	tw_orig_initWithMTLCommandQueueOptions = (init2_t)tw_swap(ci,
	    sel_registerName("initWithMTLCommandQueue:options:"), (IMP)tw_initWithMTLCommandQueueOptions);
	fprintf(stderr, "cicpu: CIContext hooks installed (gpu=%d)\n", tw_gpu_enabled());
}

static void
tw_on_add_image(const struct mach_header *mh, intptr_t slide)
{
	(void)mh;
	(void)slide;
	tw_install();
}

__attribute__((constructor)) static void
tw_ctor(void)
{
	tw_install();
	if (!tw_installed) {
		_dyld_register_func_for_add_image(tw_on_add_image);
	}
}
