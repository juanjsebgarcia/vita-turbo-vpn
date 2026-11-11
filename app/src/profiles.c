/*
 * profiles.c - the profile list read from the storages and the
 * file side of QR import. See profiles.h.
 *
 * Each storage's folder holds .conf files, sub-folders of .conf files (one
 * level, each a group) and .zip files (each a group of the .conf files
 * inside). Names are sorted without regard to case; files whose names
 * start with a dot (copies' metadata from some desktop systems) are
 * skipped. A file is read into one buffer, parsed, and the buffer and the
 * parse result are wiped before the next.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "profiles.h"
#include "vtconf.h"

#ifdef __vita__
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

const char *const PF_STORE_NAME[PF_NSTORES] = { "ux0", "uma0", "imc0", "ur0" };

static const char *const k_console_dirs[PF_NSTORES] = {
	"ux0:" PF_FOLDER, "uma0:" PF_FOLDER, "imc0:" PF_FOLDER, "ur0:" PF_FOLDER,
};

#define PATH_MAX_LEN 512

typedef struct Entry {
	BkProfile p;
	char real[PATH_MAX_LEN];  /* the file, or the zip it is in */
	int zip_index;            /* entry in the zip, -1 = a loose file */
} Entry;

static const char *g_dirs[PF_NSTORES] = {
	"ux0:" PF_FOLDER, "uma0:" PF_FOLDER, "imc0:" PF_FOLDER, "ur0:" PF_FOLDER,
};
static Entry g_e[BK_MAX_PROFILES];
static int g_n;
static unsigned g_storages;
static uint32_t g_wlan_ip;
static uint8_t g_wlan_prefix;

static char g_text[PF_TEXT_MAX];  /* the profile being read; wiped after */

/* ---- files ------------------------------------------------------------- */

/* a "/" b into out; 0, or -1 if it does not fit (the file is then skipped) */
static int join(char *out, size_t n, const char *a, const char *b)
{
	int r = snprintf(out, n, "%s/%s", a, b);
	return r < 0 || (size_t)r >= n ? -1 : 0;
}

typedef struct Name {
	char s[256];
	int dir;
} Name;

typedef struct NameList {
	Name *v;
	int n, cap;
} NameList;

static void names_add(NameList *l, const char *s, int dir)
{
	if (strlen(s) >= sizeof(l->v[0].s))
		return;
	if (l->n == l->cap) {
		int cap = l->cap ? l->cap * 2 : 64;
		Name *v = realloc(l->v, sizeof(Name) * (size_t)cap);
		if (!v)
			return;
		l->v = v;
		l->cap = cap;
	}
	strcpy(l->v[l->n].s, s);
	l->v[l->n].dir = dir;
	l->n++;
}

static void names_free(NameList *l)
{
	free(l->v);
	memset(l, 0, sizeof(*l));
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int cmp_ci(const char *a, const char *b)
{
	for (; *a && lower((unsigned char)*a) == lower((unsigned char)*b); a++, b++)
		;
	int d = lower((unsigned char)*a) - lower((unsigned char)*b);
	return d ? d : 0;
}

static int cmp_name(const void *a, const void *b)
{
	const Name *x = a, *y = b;
	int d = cmp_ci(x->s, y->s);
	return d ? d : strcmp(x->s, y->s);
}

static int ends_ci(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);
	return n > m && !cmp_ci(s + n - m, suffix);
}

/* The entries of a folder, sorted; dot-files skipped. 0, or -1 if absent */
static int list_dir(const char *path, NameList *out)
{
	memset(out, 0, sizeof(*out));
#ifdef __vita__
	SceUID d = sceIoDopen(path);
	if (d < 0)
		return -1;
	SceIoDirent e;
	memset(&e, 0, sizeof(e));
	while (sceIoDread(d, &e) > 0) {
		if (e.d_name[0] != '.')
			names_add(out, e.d_name, SCE_S_ISDIR(e.d_stat.st_mode));
		memset(&e, 0, sizeof(e));
	}
	sceIoDclose(d);
#else
	DIR *d = opendir(path);
	if (!d)
		return -1;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		char full[PATH_MAX_LEN];
		struct stat st;
		if (join(full, sizeof(full), path, e->d_name) != 0)
			continue;
		names_add(out, e->d_name, stat(full, &st) == 0 && S_ISDIR(st.st_mode));
	}
	closedir(d);
#endif
	if (out->n > 1)
		qsort(out->v, (size_t)out->n, sizeof(Name), cmp_name);
	return 0;
}

static long file_size(const char *path)
{
#ifdef __vita__
	SceIoStat st;
	if (sceIoGetstat(path, &st) < 0 || SCE_S_ISDIR(st.st_mode))
		return -1;
	return (long)st.st_size;
#else
	struct stat st;
	if (stat(path, &st) != 0 || S_ISDIR(st.st_mode))
		return -1;
	return (long)st.st_size;
#endif
}

/* Read up to cap bytes of a file; its length, or -1 */
static long read_file(const char *path, void *buf, size_t cap)
{
	long got = 0;
#ifdef __vita__
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return -1;
	while ((size_t)got < cap) {
		int r = sceIoRead(fd, (char *)buf + got, (SceSize)(cap - (size_t)got));
		if (r <= 0)
			break;
		got += r;
	}
	sceIoClose(fd);
#else
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	got = (long)fread(buf, 1, cap, f);
	fclose(f);
#endif
	return got;
}

static int write_file(const char *path, const void *buf, size_t len)
{
#ifdef __vita__
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return -1;
	int ok = sceIoWrite(fd, buf, (SceSize)len) == (int)len;
	ok &= sceIoClose(fd) >= 0;
#else
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	int ok = fwrite(buf, 1, len, f) == len;
	ok &= fclose(f) == 0;
#endif
	if (!ok) {
#ifdef __vita__
		sceIoRemove(path);
#else
		remove(path);
#endif
		return -1;
	}
	return 0;
}

static void make_dir(const char *path)
{
#ifdef __vita__
	sceIoMkdir(path, 0777);
#else
	mkdir(path, 0777);
#endif
}

/* Create path and every folder above it that is missing */
static void make_dirs(const char *path)
{
	char p[PATH_MAX_LEN];
	snprintf(p, sizeof(p), "%s", path);
	for (char *s = p + 1; *s; s++)
		if (*s == '/' && s[-1] != ':') {
			*s = 0;
			make_dir(p);
			*s = '/';
		}
	make_dir(p);
}

/* ---- parsing ----------------------------------------------------------- */

static VtcResult g_res;
static VtProfile g_vp;

static int b64_char(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		c == '+' || c == '/' || c == '=';
}

/*
 * Whether the text the parser quotes in an error could be key material: a
 * line before any section or a bare key pasted on its own line is quoted
 * as it stands, and that can be a private key. Any run of 20 base64
 * characters counts; the message then names the line only.
 */
static int may_hold_key(const char *s)
{
	int run = 0;
	for (; *s; s++) {
		run = b64_char(*s) ? run + 1 : 0;
		if (run >= 20)
			return 1;
	}
	return 0;
}

int pf_fill(const char *text, size_t len, uint32_t wlan_ip, uint8_t wlan_prefix, BkProfile *p)
{
	p->warnings = 0;
	p->key_id = 0;
	p->ignored_lines = 0;
	p->invalid_reason[0] = 0;
	p->clash_prefix = -1;
	if (vtc_parse(text, len, &g_res) != VTC_OK) {
		p->warnings |= BK_WARN_INVALID;
		if (may_hold_key(g_res.error_item))
			vtc_wipe(g_res.error_item, sizeof(g_res.error_item));
		vtc_format_error(&g_res, p->invalid_reason, sizeof(p->invalid_reason));
		vtc_wipe(&g_res, sizeof(g_res));
		return -1;
	}
	p->address = g_res.address;
	p->address_prefix = g_res.address_prefix;
	p->allowed_count = g_res.allowed_count > BK_MAX_ALLOWED ? BK_MAX_ALLOWED : g_res.allowed_count;
	for (int i = 0; i < p->allowed_count; i++) {
		p->allowed[i].net = g_res.allowed[i].net;
		p->allowed[i].prefix = g_res.allowed[i].prefix;
	}
	p->full_tunnel = g_res.full_tunnel;
	snprintf(p->endpoint_host, sizeof(p->endpoint_host), "%s", g_res.endpoint_host);
	p->endpoint_port = g_res.endpoint_port;
	p->dns[0] = g_res.dns_count > 0 ? g_res.dns[0] : 0;
	p->dns[1] = g_res.dns_count > 1 ? g_res.dns[1] : 0;
	p->mtu = g_res.mtu ? g_res.mtu : 1420;
	p->keepalive_s = g_res.keepalive_s;
	if (g_res.check == VTC_CHECK_URL) {
		p->check = BK_CHECK_URL;
		snprintf(p->check_url, sizeof(p->check_url), "%s", g_res.check_url);
	} else {
		p->check = BK_CHECK_GENERIC;
		p->check_url[0] = 0;
	}
	/* FNV-1a over the private key: only to compare two profiles */
	uint32_t h = 2166136261u;
	for (int i = 0; i < VT_KEY_LEN; i++)
		h = (h ^ g_res.private_key[i]) * 16777619u;
	p->key_id = h ? h : 1;
	p->ignored_lines = g_res.note_count + g_res.notes_dropped;
	if (p->ignored_lines)
		p->warnings |= BK_WARN_IGNORED;
	if (wlan_prefix > 0 && wlan_prefix <= 32 && vtc_to_profile(&g_res, "", &g_vp) == 0) {
		uint32_t mask = wlan_prefix == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> wlan_prefix);
		unsigned k = 0;
		if (vtc_subnet_clash(&g_vp, wlan_ip & mask, wlan_prefix, &k) != VTC_CLASH_NONE) {
			p->warnings |= BK_WARN_CLASH;
			p->clash_prefix = (int)k;
		}
	}
	vtc_wipe(&g_vp, sizeof(g_vp));
	vtc_wipe(&g_res, sizeof(g_res));
	return 0;
}

/* ---- the scan ---------------------------------------------------------- */

/* name without its ".conf", cut to fit */
static void base_name(const char *file, char *out, size_t n)
{
	const char *slash = strrchr(file, '/');
	if (slash)
		file = slash + 1;
	size_t len = strlen(file);
	if (ends_ci(file, ".conf"))
		len -= 5;
	if (len >= n)
		len = n - 1;
	/* never cut a UTF-8 character in two */
	while (len > 0 && ((unsigned char)file[len] & 0xC0) == 0x80)
		len--;
	memcpy(out, file, len);
	out[len] = 0;
}

static Entry *add_entry(int store, const char *group, int in_zip, const char *shown_rel,
	const char *real, int zip_index)
{
	if (g_n >= BK_MAX_PROFILES)
		return NULL;
	Entry *e = &g_e[g_n];
	memset(e, 0, sizeof(*e));
	base_name(shown_rel, e->p.name, sizeof(e->p.name));
	snprintf(e->p.group, sizeof(e->p.group), "%s", group);
	snprintf(e->p.storage, sizeof(e->p.storage), "%s", PF_STORE_NAME[store]);
	snprintf(e->p.path, sizeof(e->p.path), "%s:" PF_FOLDER "/%s", PF_STORE_NAME[store], shown_rel);
	e->p.in_zip = in_zip;
	e->p.clash_prefix = -1;
	snprintf(e->real, sizeof(e->real), "%s", real);
	e->zip_index = zip_index;
	return e;
}

static void invalid(Entry *e, const char *why)
{
	e->p.warnings = BK_WARN_INVALID;
	snprintf(e->p.invalid_reason, sizeof(e->p.invalid_reason), "%s", why);
}

static void read_loose(int store, const char *group, const char *rel, const char *real)
{
	Entry *e = add_entry(store, group, 0, rel, real, -1);
	if (!e)
		return;
	g_n++;
	g_storages |= 1u << store;
	long size = file_size(real);
	if (size > PF_TEXT_MAX) {
		invalid(e, "the file is larger than 16 KB; it is not a profile");
		return;
	}
	long got = size < 0 ? -1 : read_file(real, g_text, sizeof(g_text));
	if (got < 0) {
		invalid(e, "the file could not be read");
		return;
	}
	pf_fill(g_text, (size_t)got, g_wlan_ip, g_wlan_prefix, &e->p);
	vtc_wipe(g_text, (size_t)got);
}

/* A zip read whole into memory; NULL if it cannot be */
static void *zip_open(const char *real, mz_zip_archive *z, size_t *size)
{
	long n = file_size(real);
	if (n <= 0 || n > PF_ZIP_MAX)
		return NULL;
	void *buf = malloc((size_t)n);
	if (!buf)
		return NULL;
	if (read_file(real, buf, (size_t)n) != n) {
		free(buf);
		return NULL;
	}
	memset(z, 0, sizeof(*z));
	if (!mz_zip_reader_init_mem(z, buf, (size_t)n, 0)) {
		vtc_wipe(buf, (size_t)n);
		free(buf);
		return NULL;
	}
	*size = (size_t)n;
	return buf;
}

static void zip_close(mz_zip_archive *z, void *buf, size_t size)
{
	mz_zip_reader_end(z);
	vtc_wipe(buf, size);
	free(buf);
}

/* An entry's text into g_text; its length, or -1 with why set */
static long zip_read(mz_zip_archive *z, int i, const char **why)
{
	mz_zip_archive_file_stat st;
	if (!mz_zip_reader_file_stat(z, (mz_uint)i, &st)) {
		*why = "the zip entry could not be read";
		return -1;
	}
	if (st.m_is_encrypted) {
		*why = "the zip entry is encrypted";
		return -1;
	}
	if (st.m_uncomp_size > PF_TEXT_MAX) {
		*why = "the file is larger than 16 KB; it is not a profile";
		return -1;
	}
	if (!mz_zip_reader_extract_to_mem(z, (mz_uint)i, g_text, sizeof(g_text), 0)) {
		vtc_wipe(g_text, sizeof(g_text));
		*why = "the zip entry could not be unpacked";
		return -1;
	}
	return (long)st.m_uncomp_size;
}

/* A .conf entry, not a folder or a desktop system's metadata */
static int zip_wanted(mz_zip_archive *z, int i, char *name, size_t n)
{
	if (mz_zip_reader_is_file_a_directory(z, (mz_uint)i))
		return 0;
	if (!mz_zip_reader_get_filename(z, (mz_uint)i, name, (mz_uint)n))
		return 0;
	const char *slash = strrchr(name, '/');
	const char *base = slash ? slash + 1 : name;
	return base[0] != '.' && strncmp(name, "__MACOSX/", 9) != 0 && ends_ci(base, ".conf");
}

static void read_zip(int store, const char *zip_name, const char *real)
{
	mz_zip_archive z;
	size_t size;
	void *buf = zip_open(real, &z, &size);
	if (!buf) {
		Entry *e = add_entry(store, zip_name, 1, zip_name, real, -1);
		if (e) {
			/* listed so the user sees why nothing from it appears */
			snprintf(e->p.name, sizeof(e->p.name), "%s", zip_name);
			invalid(e, "the zip file could not be opened");
			g_n++;
			g_storages |= 1u << store;
		}
		return;
	}
	int count = (int)mz_zip_reader_get_num_files(&z);
	for (int i = 0; i < count && g_n < BK_MAX_PROFILES; i++) {
		char name[256], rel[PATH_MAX_LEN];
		if (!zip_wanted(&z, i, name, sizeof(name)))
			continue;
		if (join(rel, sizeof(rel), zip_name, name) != 0)
			continue;
		Entry *e = add_entry(store, zip_name, 1, rel, real, i);
		g_n++;
		g_storages |= 1u << store;
		const char *why = NULL;
		long got = zip_read(&z, i, &why);
		if (got < 0) {
			invalid(e, why);
			continue;
		}
		pf_fill(g_text, (size_t)got, g_wlan_ip, g_wlan_prefix, &e->p);
		vtc_wipe(g_text, (size_t)got);
	}
	zip_close(&z, buf, size);
}

static void read_store(int store)
{
	const char *dir = g_dirs[store];
	NameList top;
	if (!dir || list_dir(dir, &top) != 0)
		return;
	char real[PATH_MAX_LEN];
	/* loose files first, then each folder, then each zip */
	for (int i = 0; i < top.n; i++)
		if (!top.v[i].dir && ends_ci(top.v[i].s, ".conf") &&
		    join(real, sizeof(real), dir, top.v[i].s) == 0)
			read_loose(store, "", top.v[i].s, real);
	for (int i = 0; i < top.n; i++) {
		if (!top.v[i].dir)
			continue;
		char sub[PATH_MAX_LEN];
		NameList in;
		if (join(sub, sizeof(sub), dir, top.v[i].s) != 0 || list_dir(sub, &in) != 0)
			continue;
		for (int k = 0; k < in.n; k++)
			if (!in.v[k].dir && ends_ci(in.v[k].s, ".conf")) {
				char rel[PATH_MAX_LEN];
				if (join(rel, sizeof(rel), top.v[i].s, in.v[k].s) == 0 &&
				    join(real, sizeof(real), sub, in.v[k].s) == 0)
					read_loose(store, top.v[i].s, rel, real);
			}
		names_free(&in);
	}
	for (int i = 0; i < top.n; i++)
		if (!top.v[i].dir && ends_ci(top.v[i].s, ".zip") &&
		    join(real, sizeof(real), dir, top.v[i].s) == 0)
			read_zip(store, top.v[i].s, real);
	names_free(&top);
}

void pf_set_dirs(const char *const dirs[PF_NSTORES])
{
	for (int i = 0; i < PF_NSTORES; i++)
		g_dirs[i] = dirs ? dirs[i] : k_console_dirs[i];
}

void pf_scan(uint32_t wlan_ip, uint8_t wlan_prefix)
{
	g_wlan_ip = wlan_ip;
	g_wlan_prefix = wlan_prefix;
	g_n = 0;
	g_storages = 0;
	for (int s = 0; s < PF_NSTORES; s++)
		read_store(s);
}

int pf_count(void)
{
	return g_n;
}

const BkProfile *pf_get(int i)
{
	return i >= 0 && i < g_n ? &g_e[i].p : NULL;
}

unsigned pf_storages(void)
{
	static const unsigned bits[PF_NSTORES] = { BK_STORE_UX0, BK_STORE_UMA0, BK_STORE_IMC0, BK_STORE_UR0 };
	unsigned out = 0;
	for (int s = 0; s < PF_NSTORES; s++)
		if (g_storages & 1u << s)
			out |= bits[s];
	return out;
}

const char *pf_source(int i, int *zip_index)
{
	if (i < 0 || i >= g_n || (g_e[i].p.in_zip && g_e[i].zip_index < 0))
		return NULL;
	*zip_index = g_e[i].zip_index;
	return g_e[i].real;
}

int pf_find(const char *real, int zip_index)
{
	for (int i = 0; i < g_n; i++)
		if (g_e[i].zip_index == zip_index && !strcmp(g_e[i].real, real))
			return i;
	return -1;
}

long pf_read_source(const char *real, int zip_index, char *buf, size_t cap)
{
	if (zip_index < 0) {
		long size = file_size(real);
		if (size < 0 || (size_t)size > cap)
			return -1;
		return read_file(real, buf, cap);
	}
	mz_zip_archive z;
	size_t zsize;
	void *zbuf = zip_open(real, &z, &zsize);
	if (!zbuf)
		return -1;
	long got = -1;
	mz_zip_archive_file_stat st;
	if (mz_zip_reader_file_stat(&z, (mz_uint)zip_index, &st) && !st.m_is_encrypted &&
	    st.m_uncomp_size <= cap &&
	    mz_zip_reader_extract_to_mem(&z, (mz_uint)zip_index, buf, cap, 0))
		got = (long)st.m_uncomp_size;
	else
		vtc_wipe(buf, cap);
	zip_close(&z, zbuf, zsize);
	return got;
}

void pf_make_folder(void)
{
	if (g_dirs[PF_UX0])
		make_dirs(g_dirs[PF_UX0]);
}

/* ---- QR import --------------------------------------------------- */

/* The listed profile whose text is the same as text, or -1 */
static int find_same(const char *text, size_t len)
{
	int found = -1;
	mz_zip_archive z;
	void *zbuf = NULL;
	size_t zsize = 0;
	const char *zreal = NULL;
	for (int i = 0; i < g_n && found < 0; i++) {
		const Entry *e = &g_e[i];
		long got;
		if (e->zip_index < 0) {
			if (e->p.in_zip)
				continue;   /* a zip that could not be opened */
			long size = file_size(e->real);
			if (size < 0 || size > PF_TEXT_MAX)
				continue;
			got = read_file(e->real, g_text, sizeof(g_text));
		} else {
			if (!zreal || strcmp(zreal, e->real)) {
				if (zbuf)
					zip_close(&z, zbuf, zsize);
				zreal = e->real;
				zbuf = zip_open(e->real, &z, &zsize);
			}
			const char *why;
			got = zbuf ? zip_read(&z, e->zip_index, &why) : -1;
		}
		if (got >= 0) {
			if (vtc_same_text(text, len, g_text, (size_t)got))
				found = i;
			vtc_wipe(g_text, (size_t)got);
		}
	}
	if (zbuf)
		zip_close(&z, zbuf, zsize);
	return found;
}

static int name_taken(void *ctx, const char *file_name)
{
	char path[PATH_MAX_LEN];
	(void)ctx;
	if (join(path, sizeof(path), g_dirs[PF_UX0], file_name) != 0)
		return 1;
#ifdef __vita__
	SceIoStat st;
	return sceIoGetstat(path, &st) >= 0;
#else
	struct stat st;
	return stat(path, &st) == 0;
#endif
}

int pf_import(const char *text, size_t len, BkImport *out)
{
	static BkProfile parsed;
	memset(out, 0, sizeof(*out));
	out->profile = -1;
	if (pf_fill(text, len, g_wlan_ip, g_wlan_prefix, &parsed) != 0) {
		out->result = BK_IMPORT_INVALID;
		snprintf(out->reason, sizeof(out->reason), "%s", parsed.invalid_reason);
		return -1;
	}
	int same = find_same(text, len);
	if (same >= 0) {
		const BkProfile *p = &g_e[same].p;
		out->result = BK_IMPORT_DUPLICATE;
		out->profile = same;
		snprintf(out->name, sizeof(out->name), "%s", p->nickname[0] ? p->nickname : p->name);
		return 0;
	}
	char file[VTC_FILE_NAME_MAX], path[PATH_MAX_LEN];
	out->result = BK_IMPORT_FAILED;
	if (!g_dirs[PF_UX0])
		return -1;
	make_dirs(g_dirs[PF_UX0]);
	if (vtc_qr_file_name(parsed.endpoint_host, name_taken, NULL, file, sizeof(file)) != 0)
		return -1;
	if (join(path, sizeof(path), g_dirs[PF_UX0], file) != 0)
		return -1;
	if (write_file(path, text, len) != 0)
		return -1;
	out->result = BK_IMPORT_SAVED;
	base_name(file, out->name, sizeof(out->name));
	pf_scan(g_wlan_ip, g_wlan_prefix);
	for (int i = 0; i < g_n; i++)
		if (!strcmp(g_e[i].real, path)) {
			out->profile = i;
			break;
		}
	return 0;
}
