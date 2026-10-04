/* Workaround for Wine 11.18 on macOS: win32u's parse_current_extensions()
 * queries glGetIntegerv(GL_MAJOR_VERSION) on every new context. On macOS's
 * legacy (2.1) OpenGL profile that's GL_INVALID_ENUM, which Wine leaves
 * pending for the app (PCLink: "Error initializing OpenGL! invalid
 * enumerant"); the 2.x fallback path then fills a shadowed local array, so
 * apps see no GL extensions at all. Both are fixed in Wine master.
 *
 * Injected with DYLD_INSERT_LIBRARIES, this answers the GL3 queries from
 * the legacy extension string on 2.x contexts, steering Wine onto its
 * indexed-extension path, which works. Contexts >= 3.0 pass straight
 * through. Build for x86_64: Wine runs under Rosetta. */
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <stdlib.h>
#include <string.h>

#define DYLD_INTERPOSE(_new, _old) \
    __attribute__((used)) static struct { const void *n; const void *o; } interpose_##_old \
    __attribute__((section("__DATA,__interpose"))) = { (const void *)&_new, (const void *)&_old };

static int legacy_context(void)
{
    const char *version = (const char *)glGetString(GL_VERSION);
    return version && atoi(version) < 3;
}

/* Find the index-th space-separated name in the legacy extension string. */
static const char *nth_extension(GLuint index, size_t *len)
{
    const char *p = (const char *)glGetString(GL_EXTENSIONS);
    GLuint i = 0;

    if (!p) return NULL;
    for (;;)
    {
        while (*p == ' ') p++;
        if (!*p) return NULL;
        *len = strcspn(p, " ");
        if (i++ == index) return p;
        p += *len;
    }
}

static void fix_glGetIntegerv(GLenum pname, GLint *data)
{
    if ((pname == GL_MAJOR_VERSION || pname == GL_NUM_EXTENSIONS) && legacy_context())
    {
        size_t len;
        GLint n = 0;

        if (pname == GL_MAJOR_VERSION)
        {
            *data = 3;  /* only Wine asks this on a 2.x context; selects its working path */
            return;
        }
        while (nth_extension((GLuint)n, &len)) n++;
        *data = n;
        return;
    }
    glGetIntegerv(pname, data);
}
DYLD_INTERPOSE(fix_glGetIntegerv, glGetIntegerv)

static const GLubyte *fix_glGetStringi(GLenum name, GLuint index)
{
    static __thread char buf[256];
    const char *ext;
    size_t len;

    if (name != GL_EXTENSIONS || !legacy_context()) return glGetStringi(name, index);
    if (!(ext = nth_extension(index, &len)) || len >= sizeof(buf)) return NULL;
    memcpy(buf, ext, len);
    buf[len] = 0;
    return (const GLubyte *)buf;
}
DYLD_INTERPOSE(fix_glGetStringi, glGetStringi)
