#ifndef _HAVE_UTIL_SHORTJSON_H
#define _HAVE_UTIL_SHORTJSON_H

typedef json_object *JSON;

/* Creates JSON object with refcount of 1.
 */
static __inline__ JSON
Jnew (void)
{
    JSON n = json_object_new_object ();
    if (!n)
        oom ();
    return n;
}

/* Increment JSON object refcount.
 */
static __inline__ JSON
Jget (JSON o)
{
    return o ? json_object_get (o) : o;
}

/* Decrement JSON object refcount and free if refcount == 0.
 */
static __inline__ void
Jput (JSON o)
{
    if (o)
        json_object_put (o);
}

/* Add integer to JSON.
 */
static __inline__ void
Jadd_int (JSON o, const char *name, int i)
{
    JSON n = json_object_new_int (i);
    if (!n)
        oom ();
    json_object_object_add (o, (char *)name, n);
}

/* Add string to JSON (caller retains ownership of original).
 */
static __inline__ void
Jadd_str (JSON o, const char *name, const char *s)
{
    JSON n = json_object_new_string (s);
    if (!n)
        oom ();
    json_object_object_add (o, (char *)name, n);
}

/* Add object to JSON (caller retains ownership of original).
 */
static __inline__ void
Jadd_obj (JSON o, const char *name, JSON obj)
{
    json_object_object_add (o, (char *)name, Jget (obj));
}

/* Get integer from JSON.
 */
static __inline__ bool
Jget_int (JSON o, const char *name, int *ip)
{
    JSON n = json_object_object_get (o, (char *)name);
    if (n)
        *ip = json_object_get_int (n);
    return (n != NULL);
}

/* Get string from JSON (still owned by JSON, do not free).
 */
static __inline__ bool
Jget_str (JSON o, const char *name, const char **sp)
{
    JSON n = json_object_object_get (o, (char *)name);
    if (n)
        *sp = json_object_get_string (n);
    return (n != NULL);
}

/* Get object from JSON (still owned by JSON, do not free).
 */
static __inline__ bool
Jget_obj (JSON o, const char *name, JSON *op)
{
    JSON n = json_object_object_get (o, (char *)name);
    if (n)
        *op = n;
    return (n != NULL);
}

/* Get boolean from JSON.
 */
static __inline__ bool
Jget_bool (JSON o, const char *name, bool *bp)
{
    JSON n = json_object_object_get (o, (char *)name);
    if (n)
        *bp = json_object_get_boolean (n);
    return (n != NULL);
}

/* Encode JSON to string (owned by JSON, do not free)
 */
static __inline__ const char *
Jtostr (JSON o)
{
    return o ? json_object_to_json_string (o) : NULL;
}

/* Decode string to JSON (caller is given ownership).
 */
static __inline__ JSON
Jfromstr (const char *s)
{
    struct json_tokener *tok;
    JSON o = NULL;

    if (!(tok = json_tokener_new ()))
        oom ();
    o = json_tokener_parse_ex (tok, s, strlen (s));
    json_tokener_free (tok);
    return o;
}

static __inline__ void
Jmerge (JSON dst, JSON src)
{
    json_object_iter iter;

    json_object_object_foreachC (src, iter) {
        json_object_object_del (dst, iter.key); /* necessary? */
        json_object_object_add (dst, iter.key, Jget (iter.val));
    }
}

#endif
/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */