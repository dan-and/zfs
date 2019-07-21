/*
 * BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer
 * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 2016-2018 by Klara Systems Inc.
 * Copyright (c) 2016-2018 Allan Jude <allanjude@freebsd.org>
 * Copyright (c) 2018 Sebastian Gottschall <s.gottschall@dd-wrt.com>
 */

#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/zfs_context.h>
#include <sys/zio_compress.h>
#include <sys/spa.h>

#define	ZSTD_STATIC_LINKING_ONLY
#include <sys/zstd/zstd.h>
#include <sys/zstd/zstd_errors.h>
#include <sys/zstd/error_private.h>


#define	ZSTD_KMEM_MAGIC		0x20160831

/* for BSD compat */
#define	__unused			__attribute__((unused))


#ifdef _KERNEL
#include <linux/sort.h>
#define	zstd_qsort(a, n, es, cmp) sort(a, n, es, cmp, NULL)
#else
#define	zstd_qsort qsort
#endif

static size_t real_zstd_compress(const char *source, char *dest, int isize,
    int osize, int level);
static size_t real_zstd_decompress(const char *source, char *dest, int isize,
    int maxosize);

static void *zstd_alloc(void *opaque, size_t size);
static void zstd_free(void *opaque, void *ptr);

static const ZSTD_customMem zstd_malloc = {
	zstd_alloc,
	zstd_free,
	NULL,
};
/* these enums are index references to zstd_cache_config */

enum zstd_kmem_type {
	ZSTD_KMEM_UNKNOWN = 0,
	ZSTD_KMEM_CCTX,
	ZSTD_KMEM_WRKSPC_4K_MIN,
	ZSTD_KMEM_WRKSPC_4K_DEF,
	ZSTD_KMEM_WRKSPC_4K_MAX,
	ZSTD_KMEM_WRKSPC_16K_MIN,
	ZSTD_KMEM_WRKSPC_16K_DEF,
	ZSTD_KMEM_WRKSPC_16K_MAX,
	ZSTD_KMEM_WRKSPC_128K_MIN,
	ZSTD_KMEM_WRKSPC_128K_DEF,
	ZSTD_KMEM_WRKSPC_128K_MAX,
	ZSTD_KMEM_WRKSPC_16M_MIN,
	ZSTD_KMEM_WRKSPC_16M_DEF,
	ZSTD_KMEM_WRKSPC_16M_MAX,
	ZSTD_KMEM_DCTX,
	ZSTD_KMEM_COUNT,
};

struct zstd_kmem {
	uint_t			kmem_magic;
	enum zstd_kmem_type	kmem_type;
	size_t			kmem_size;
	int			kmem_flags;
	boolean_t		isvm;
};

struct zstd_vmem {
	size_t			vmem_size;
	void			*vm;
	kmutex_t 		barrier;
	boolean_t		inuse;
};

struct zstd_kmem_config {
	size_t			block_size;
	int			compress_level;
	char			*cache_name;
	int 			flags;
};

static kmem_cache_t *zstd_kmem_cache[ZSTD_KMEM_COUNT] = { NULL };
static struct zstd_kmem zstd_cache_size[ZSTD_KMEM_COUNT] = {
	{ ZSTD_KMEM_MAGIC, 0, 0, KM_NOSLEEP, B_FALSE} };

static struct zstd_vmem zstd_vmem_cache[ZSTD_KMEM_COUNT] = {
		{
		.vmem_size = 0,
		.vm = NULL,
		.inuse = B_FALSE
		}
	};
static struct zstd_kmem_config zstd_cache_config[ZSTD_KMEM_COUNT] = {
	{ 0, 0, "zstd_unknown", KM_SLEEP},
	{ 0, 0, "zstd_cctx", KM_NOSLEEP },
	{ 4096, ZIO_ZSTD_LEVEL_MIN, "zstd_wrkspc_4k_min", KM_NOSLEEP},
	{ 4096, ZIO_ZSTD_LEVEL_DEFAULT, "zstd_wrkspc_4k_def", KM_NOSLEEP},
	{ 4096, ZIO_ZSTD_LEVEL_MAX, "zstd_wrkspc_4k_max", KM_NOSLEEP},
	{ 16384, ZIO_ZSTD_LEVEL_MIN, "zstd_wrkspc_16k_min", KM_NOSLEEP},
	{ 16384, ZIO_ZSTD_LEVEL_DEFAULT, "zstd_wrkspc_16k_def", KM_NOSLEEP},
	{ 16384, ZIO_ZSTD_LEVEL_MAX, "zstd_wrkspc_16k_max", KM_NOSLEEP},
	{ SPA_OLD_MAXBLOCKSIZE, ZIO_ZSTD_LEVEL_MIN, \
	    "zstd_wrkspc_128k_min", KM_NOSLEEP},
	{ SPA_OLD_MAXBLOCKSIZE, ZIO_ZSTD_LEVEL_DEFAULT,
	    "zstd_wrkspc_128k_def", KM_NOSLEEP},
	{ SPA_OLD_MAXBLOCKSIZE, ZIO_ZSTD_LEVEL_MAX, \
	    "zstd_wrkspc_128k_max", KM_NOSLEEP},
	{ SPA_MAXBLOCKSIZE, ZIO_ZSTD_LEVEL_MIN, \
	    "zstd_wrkspc_16m_min", KM_NOSLEEP},
	{ SPA_MAXBLOCKSIZE, ZIO_ZSTD_LEVEL_DEFAULT, \
	    "zstd_wrkspc_16m_def", KM_NOSLEEP},
	{ SPA_MAXBLOCKSIZE, ZIO_ZSTD_LEVEL_MAX, \
	    "zstd_wrkspc_16m_max", KM_NOSLEEP},
	{ 0, 0, "zstd_dctx", KM_SLEEP},
};

static int
zstd_compare(const void *a, const void *b)
{
	struct zstd_kmem *x, *y;

	x = (struct zstd_kmem *)a;
	y = (struct zstd_kmem *)b;

	ASSERT3U(x->kmem_magic, ==, ZSTD_KMEM_MAGIC);
	ASSERT3U(y->kmem_magic, ==, ZSTD_KMEM_MAGIC);

	return (AVL_CMP(x->kmem_size, y->kmem_size));
}

static enum zio_zstd_levels
zstd_level_to_enum(int level)
{
	enum zio_zstd_levels elevel = ZIO_ZSTDLVL_INHERIT;

	if (level > 0 && level <= ZIO_ZSTDLVL_MAX) {
		elevel = level;
		return (elevel);
	} else if (level < 0) {
		switch (level) {
			case -1:
				return (ZIO_ZSTDLVL_FAST_1);
			case -2:
				return (ZIO_ZSTDLVL_FAST_2);
			case -3:
				return (ZIO_ZSTDLVL_FAST_3);
			case -4:
				return (ZIO_ZSTDLVL_FAST_4);
			case -5:
				return (ZIO_ZSTDLVL_FAST_5);
			case -6:
				return (ZIO_ZSTDLVL_FAST_6);
			case -7:
				return (ZIO_ZSTDLVL_FAST_7);
			case -8:
				return (ZIO_ZSTDLVL_FAST_8);
			case -9:
				return (ZIO_ZSTDLVL_FAST_9);
			case -10:
				return (ZIO_ZSTDLVL_FAST_10);
			case -20:
				return (ZIO_ZSTDLVL_FAST_20);
			case -30:
				return (ZIO_ZSTDLVL_FAST_30);
			case -40:
				return (ZIO_ZSTDLVL_FAST_40);
			case -50:
				return (ZIO_ZSTDLVL_FAST_50);
			case -60:
				return (ZIO_ZSTDLVL_FAST_60);
			case -70:
				return (ZIO_ZSTDLVL_FAST_70);
			case -80:
				return (ZIO_ZSTDLVL_FAST_80);
			case -90:
				return (ZIO_ZSTDLVL_FAST_90);
			case -100:
				return (ZIO_ZSTDLVL_FAST_100);
			case -500:
				return (ZIO_ZSTDLVL_FAST_500);
			case -1000:
				return (ZIO_ZSTDLVL_FAST_1000);
		}
	}

	/* This shouldn't happen. Cause a panic. */
	panic("Invalid ZSTD level encountered: %d", level);

	return (ZIO_ZSTDLVL_INHERIT);
}

static int
zstd_enum_to_level(enum zio_zstd_levels elevel)
{
	int level = 0;

	if (elevel > ZIO_ZSTDLVL_INHERIT && elevel <= ZIO_ZSTDLVL_MAX) {
		level = elevel;
		return (level);
	} else if (elevel > ZIO_ZSTDLVL_FAST) {
		switch (elevel) {
			case ZIO_ZSTDLVL_FAST_1:
				return (-1);
			case ZIO_ZSTDLVL_FAST_2:
				return (-2);
			case ZIO_ZSTDLVL_FAST_3:
				return (-3);
			case ZIO_ZSTDLVL_FAST_4:
				return (-4);
			case ZIO_ZSTDLVL_FAST_5:
				return (-5);
			case ZIO_ZSTDLVL_FAST_6:
				return (-6);
			case ZIO_ZSTDLVL_FAST_7:
				return (-7);
			case ZIO_ZSTDLVL_FAST_8:
				return (-8);
			case ZIO_ZSTDLVL_FAST_9:
				return (-9);
			case ZIO_ZSTDLVL_FAST_10:
				return (-10);
			case ZIO_ZSTDLVL_FAST_20:
				return (-20);
			case ZIO_ZSTDLVL_FAST_30:
				return (-30);
			case ZIO_ZSTDLVL_FAST_40:
				return (-40);
			case ZIO_ZSTDLVL_FAST_50:
				return (-50);
			case ZIO_ZSTDLVL_FAST_60:
				return (-60);
			case ZIO_ZSTDLVL_FAST_70:
				return (-70);
			case ZIO_ZSTDLVL_FAST_80:
				return (-80);
			case ZIO_ZSTDLVL_FAST_90:
				return (-90);
			case ZIO_ZSTDLVL_FAST_100:
				return (-100);
			case ZIO_ZSTDLVL_FAST_500:
				return (-500);
			case ZIO_ZSTDLVL_FAST_1000:
				return (-1000);
			default:
			break;
		}
	}

	return (ZIO_ZSTD_LEVEL_DEFAULT);
}

size_t
zstd_compress(void *s_start, void *d_start, size_t s_len, size_t d_len, int n)
{
	size_t c_len;
	uint32_t bufsiz;
	int32_t zstdlevel;
	char *dest = d_start;

	ASSERT(d_len >= sizeof (bufsiz));
	ASSERT(d_len <= s_len);

	zstdlevel = zstd_enum_to_level(n);

	/* XXX: this could overflow, but we never have blocks that big */
	c_len = real_zstd_compress(s_start,
	    &dest[sizeof (bufsiz) + sizeof (zstdlevel)], s_len,
	    d_len - sizeof (bufsiz) - sizeof (zstdlevel), zstdlevel);

	/* Signal an error if the compression routine returned an error. */
	if (ZSTD_isError(c_len)) {
		return (s_len);
	}

	/*
	 * Encode the compresed buffer size at the start. We'll need this in
	 * decompression to counter the effects of padding which might be
	 * added to the compressed buffer and which, if unhandled, would
	 * confuse the hell out of our decompression function.
	 */
	bufsiz = c_len;
	*(uint32_t *)dest = BE_32(bufsiz);
	/*
	 * Encode the compression level as well. We may need to know the
	 * original compression level if compressed_arc is disabled, to match
	 * the compression settings to write this block to the L2ARC.
	 * Encode the actual level, so if the enum changes in the future,
	 * we will be compatible.
	 */
	*(uint32_t *)(&dest[sizeof (bufsiz)]) = BE_32(zstdlevel);

	return (c_len + sizeof (bufsiz) + sizeof (zstdlevel));
}

int
zstd_decompress(void *s_start, void *d_start, size_t s_len, size_t d_len, int n)
{
	const char *src = s_start;
	uint32_t bufsiz = BE_IN32(src);
	uint32_t cookie = BE_IN32(&src[sizeof (bufsiz)]);
	int32_t zstdlevel = zstd_level_to_enum(cookie);

	ASSERT(d_len >= s_len);
	ASSERT(zstdlevel > ZIO_ZSTDLVL_INHERIT);
	ASSERT(zstdlevel < ZIO_ZSTDLVL_LEVELS);

	/* invalid compressed buffer size encoded at start */
	if (bufsiz + sizeof (bufsiz) > s_len) {
#ifdef _KERNEL
		printk(KERN_INFO "zstd: invalid compressed buffer size\n");
#endif
		return (1);
	}

	/*
	 * Returns 0 on success (decompression function returned non-negative)
	 * and non-zero on failure (decompression function returned negative.
	 */
	size_t len = real_zstd_decompress(
	    &src[sizeof (bufsiz) + sizeof (zstdlevel)], \
	    d_start, bufsiz, d_len);
	if (ZSTD_isError(len)) {
#ifdef _KERNEL
		printk(KERN_INFO "zstd: decompression failed %zu\n", len);
#endif
		return (1);
	}

	return (0);
}

int
zstd_getlevel(void *s_start, size_t s_len __unused)
{
	const char *src = s_start;
	uint32_t cookie = BE_IN32(&src[sizeof (uint32_t)]);
	int32_t zstdlevel = zstd_level_to_enum(cookie);

	ASSERT(zstdlevel > ZIO_ZSTDLVL_INHERIT);
	ASSERT(zstdlevel < ZIO_ZSTDLVL_LEVELS);

	return (zstdlevel);
}

static size_t
real_zstd_compress(const char *source, char *dest, int isize, int osize,
    int level)
{
	size_t result;
	ZSTD_CCtx *cctx;

	ASSERT(level != 0);
	if (level == ZIO_ZSTDLVL_DEFAULT)
		level = ZIO_ZSTD_LEVEL_DEFAULT;

	cctx = ZSTD_createCCtx_advanced(zstd_malloc);
	/*
	 * out of kernel memory, gently fall through - this will disable
	 * compression in zio_compress_data
	 */
	if (cctx == NULL)
		return (0);

	result = ZSTD_compressCCtx(cctx, dest, osize, source, isize, level);

	ZSTD_freeCCtx(cctx);
	return (result);
}


static size_t
real_zstd_decompress(const char *source, char *dest, int isize, int maxosize)
{
	size_t result;
	ZSTD_DCtx *dctx;

	dctx = ZSTD_createDCtx_advanced(zstd_malloc);
	if (dctx == NULL)
		return (ZSTD_error_memory_allocation);

	result = ZSTD_decompressDCtx(dctx, dest, maxosize, source, isize);

	ZSTD_freeDCtx(dctx);
	return (result);
}
static int zstd_meminit(void);



extern void *
zstd_alloc(void *opaque __unused, size_t size)
{
	size_t nbytes = sizeof (struct zstd_kmem) + size;
	struct zstd_kmem *z = NULL;
	enum zstd_kmem_type type;
	enum zstd_kmem_type newtype;
	int i;
	type = ZSTD_KMEM_UNKNOWN;
	for (i = 0; i < ZSTD_KMEM_COUNT; i++) {
		if (nbytes <= zstd_cache_size[i].kmem_size) {
			type = zstd_cache_size[i].kmem_type;
			if (zstd_kmem_cache[type]) {
				z = kmem_cache_alloc( \
				    zstd_kmem_cache[type], \
				    zstd_cache_size[i].kmem_flags);
				if (z) {
					memset(z, 0, nbytes);
					z->isvm = B_FALSE;
				}
			}
			break;
		}
	}
	newtype = type;
	/* No matching cache */
	if (type == ZSTD_KMEM_UNKNOWN || z == NULL) {
		/*
		 * consider max allocation size
		 * so we need to use standard vmem allocator
		 */
#ifdef _KERNEL
		z = vmem_zalloc(nbytes, KM_SLEEP);
#else
		z = kmem_zalloc(nbytes, KM_SLEEP);
#endif
		if (z)
			newtype = ZSTD_KMEM_UNKNOWN;
	}
	/* fallback if everything fails */
	if (!z && zstd_vmem_cache[type].vm && type == ZSTD_KMEM_DCTX) {
		mutex_enter(&zstd_vmem_cache[type].barrier);
		mutex_exit(&zstd_vmem_cache[type].barrier);

		mutex_enter(&zstd_vmem_cache[type].barrier);
		newtype = ZSTD_KMEM_DCTX;
		zstd_vmem_cache[type].inuse = B_TRUE;
		z = zstd_vmem_cache[type].vm;
		if (z) {
			memset(z, 0, nbytes);
			z->isvm = B_TRUE;
		}
	}

	/* allocation should always be successful */
	if (z == NULL) {
		return (NULL);
	}

	z->kmem_magic = ZSTD_KMEM_MAGIC;
	z->kmem_type = newtype;
	z->kmem_size = nbytes;

	return ((void*)z + (sizeof (struct zstd_kmem)));
}

extern void
zstd_free(void *opaque __unused, void *ptr)
{
	struct zstd_kmem *z = ptr - sizeof (struct zstd_kmem);
	enum zstd_kmem_type type;

	ASSERT3U(z->kmem_magic, ==, ZSTD_KMEM_MAGIC);
	ASSERT3U(z->kmem_type, <, ZSTD_KMEM_COUNT);
	ASSERT3U(z->kmem_type, >=, ZSTD_KMEM_UNKNOWN);
	type = z->kmem_type;
	if (type == ZSTD_KMEM_UNKNOWN) {
		kmem_free(z, z->kmem_size);
	} else {
		if (zstd_kmem_cache[type] && z->isvm == B_FALSE) {
			kmem_cache_free(zstd_kmem_cache[type], z);
		} else if (zstd_vmem_cache[type].vm && \
		    zstd_vmem_cache[type].inuse == B_TRUE) {
			zstd_vmem_cache[type].inuse = B_FALSE;
			/* release barrier */
			mutex_exit(&zstd_vmem_cache[type].barrier);
		}

	}
}
#ifndef _KERNEL
#define	__init
#define	__exit
#endif

static void create_vmem_cache(struct zstd_vmem *mem, char *name, size_t size)
{
#ifdef _KERNEL
	mem->vmem_size = size;
	mem->vm = \
	    vmem_zalloc(mem->vmem_size, \
	    KM_SLEEP);
	mem->inuse = B_FALSE;
	mutex_init(&mem->barrier, \
	    NULL, MUTEX_DEFAULT, NULL);
#endif
}
static int zstd_meminit(void)
{
	int i;

	/* There is no estimate function for the CCtx itself */
	zstd_cache_size[1].kmem_magic = ZSTD_KMEM_MAGIC;
	zstd_cache_size[1].kmem_type = 1;
	zstd_cache_size[1].kmem_size = P2ROUNDUP(zstd_cache_config[1].block_size
	    + sizeof (struct zstd_kmem), PAGESIZE);
	zstd_kmem_cache[1] = kmem_cache_create(
	    zstd_cache_config[1].cache_name, zstd_cache_size[1].kmem_size,
	    0, NULL, NULL, NULL, NULL, NULL, KMC_KVMEM);
	zstd_cache_size[1].kmem_flags = zstd_cache_config[1].flags;

	/*
	 * Estimate the size of the ZSTD CCtx workspace required for each record
	 * size at each compression level.
	 */
	for (i = 2; i < ZSTD_KMEM_DCTX; i++) {
		ASSERT(zstd_cache_config[i].cache_name != NULL);
		zstd_cache_size[i].kmem_magic = ZSTD_KMEM_MAGIC;
		zstd_cache_size[i].kmem_type = i;
		zstd_cache_size[i].kmem_size = P2ROUNDUP(
		    ZSTD_estimateCCtxSize_usingCParams(
		    ZSTD_getCParams(zstd_cache_config[i].compress_level,
		    zstd_cache_config[i].block_size, 0)) +
		    sizeof (struct zstd_kmem), PAGESIZE);
		zstd_cache_size[i].kmem_flags = zstd_cache_config[i].flags;
		zstd_kmem_cache[i] = kmem_cache_create(
		    zstd_cache_config[i].cache_name,
		    zstd_cache_size[i].kmem_size,
		    0, NULL, NULL, NULL, NULL, NULL, KMC_KVMEM);
	}

	/* Estimate the size of the decompression context */
	zstd_cache_size[i].kmem_magic = ZSTD_KMEM_MAGIC;
	zstd_cache_size[i].kmem_type = i;
	zstd_cache_size[i].kmem_size = P2ROUNDUP(ZSTD_estimateDCtxSize() +
	    sizeof (struct zstd_kmem), PAGESIZE);
	zstd_kmem_cache[i] = kmem_cache_create(zstd_cache_config[i].cache_name,
	    zstd_cache_size[i].kmem_size, 0, NULL, NULL, NULL, NULL, NULL,
	    KMC_KVMEM);
	zstd_cache_size[i].kmem_flags = zstd_cache_config[i].flags;


	create_vmem_cache(&zstd_vmem_cache[i], \
	    zstd_cache_config[i].cache_name, \
	    zstd_cache_size[i].kmem_size);

	/* Sort the kmem caches for later searching */
	zstd_qsort(zstd_cache_size, ZSTD_KMEM_COUNT, sizeof (struct zstd_kmem),
	    zstd_compare);

	return (0);
}

extern int __init
zstd_init(void)
{
	zstd_meminit();
	return (0);
}

extern void __exit
zstd_fini(void)
{
	int i, type;

	for (i = 0; i < ZSTD_KMEM_COUNT; i++) {
		type = zstd_cache_size[i].kmem_type;
		if (zstd_vmem_cache[type].vm) {
			kmem_free(zstd_vmem_cache[type].vm, \
			    zstd_vmem_cache[type].vmem_size);
			zstd_vmem_cache[type].vm = NULL;
			zstd_vmem_cache[type].inuse = B_FALSE;
			mutex_destroy(&zstd_vmem_cache[type].barrier);
		} else {
			if (zstd_kmem_cache[type] != NULL) {
				kmem_cache_destroy(zstd_kmem_cache[type]);
			}
		}
	}
}


#if defined(_KERNEL)
module_init(zstd_init);
module_exit(zstd_fini);
EXPORT_SYMBOL(zstd_compress);
EXPORT_SYMBOL(zstd_decompress);
EXPORT_SYMBOL(zstd_getlevel);

MODULE_DESCRIPTION("ZSTD Compression for ZFS");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("1.4.1");
#endif