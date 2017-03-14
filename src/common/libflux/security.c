/*****************************************************************************\
 *  Copyright (c) 2014 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

/* security.c - flux security functions */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <getopt.h>
#include <libgen.h>
#include <munge.h>
#include <czmq.h>

#include "security.h"
#include "flog.h"

#include "src/common/libutil/log.h"
#include "src/common/libutil/oom.h"
#include "src/common/libutil/xzmalloc.h"
#include "src/common/libutil/base64.h"


#if ZMQ_VERSION_MAJOR < 4
#error CURVE was introduced in zeromq v4
#endif

struct flux_sec_struct {
    zctx_t *zctx;
    char *domain;
    int typemask;
    zauth_t *zauth;
    zcert_t *srv_cert;
    zcert_t *cli_cert;
    munge_ctx_t mctx;
    char *conf_dir;
    char *curve_dir;
    char *passwd_file;
    char *errstr;
    char *confstr;
    uid_t uid;
    uid_t gid;
    pthread_mutex_t lock;
};

static int checksecdirs (flux_sec_t *c, bool create);
static zcert_t *getcurve (flux_sec_t *c, const char *role);
static int gencurve (flux_sec_t *c, const char *role, bool force, bool verbose);
static char *getpasswd (flux_sec_t *c, const char *user);
static int genpasswd (flux_sec_t *c, const char *user, bool force, bool verbose);

static void lock_sec (flux_sec_t *c)
{
    int e = pthread_mutex_lock (&c->lock);
    if (e)
        log_errn_exit (e, "pthread_mutex_lock");
}

static void unlock_sec (flux_sec_t *c)
{
    int e = pthread_mutex_unlock (&c->lock);
    if (e)
        log_errn_exit (e, "pthread_mutex_unlock");
}

const char *flux_sec_errstr (flux_sec_t *c)
{
    return (c->errstr ? c->errstr : "Success");
}

const char *flux_sec_confstr (flux_sec_t *c)
{
    lock_sec (c);
    if (c->confstr)
        free (c->confstr);
    if (asprintf (&c->confstr, "Security: epgm=%s, tcp/ipc=%s",
               (c->typemask & FLUX_SEC_TYPE_MUNGE) ? "MUNGE" : "off",
               (c->typemask & FLUX_SEC_TYPE_PLAIN) ? "PLAIN"
             : (c->typemask & FLUX_SEC_TYPE_CURVE) ? "CURVE" : "off") < 0)
        oom ();
    unlock_sec (c);
    return c->confstr;
}

static void seterrstr (flux_sec_t *c, const char *fmt, ...)
{
    va_list ap;

    /* XXX c->lock held */

    if (c->errstr)
        free (c->errstr);
    va_start (ap, fmt);
    if (vasprintf (&c->errstr, fmt, ap) < 0)
        oom ();
    va_end (ap);
}

void flux_sec_destroy (flux_sec_t *c)
{
    if (c->domain)
        free (c->domain);
    if (c->conf_dir)
        free (c->conf_dir);
    if (c->curve_dir)
        free (c->curve_dir);
    if (c->passwd_file)
        free (c->passwd_file);
    if (c->cli_cert)
        zcert_destroy (&c->cli_cert);
    if (c->srv_cert)
        zcert_destroy (&c->srv_cert);
    if (c->zauth)
        zauth_destroy (&c->zauth);
    if (c->mctx)
        munge_ctx_destroy (c->mctx);
    if (c->errstr)
        free (c->errstr);
    if (c->confstr)
        free (c->confstr);
    (void)pthread_mutex_destroy (&c->lock);
    free (c);
}

flux_sec_t *flux_sec_create (void)
{
    flux_sec_t *c = xzmalloc (sizeof (*c));
    int e;

    if ((e = pthread_mutex_init (&c->lock, NULL)))
        log_errn_exit (e, "pthread_mutex_init");
    c->uid = getuid ();
    c->gid = getgid ();
    c->typemask = FLUX_SEC_TYPE_MUNGE | FLUX_SEC_TYPE_CURVE;
    return c;
}

static int validate_type (int tm)
{
    /* XXX c->lock held */

    if ((tm & FLUX_SEC_TYPE_CURVE) && (tm & FLUX_SEC_TYPE_PLAIN))
        goto einval; /* both can't be enabled */
    return 0;
einval:
    errno = EINVAL;
    return -1;
}

void flux_sec_set_directory (flux_sec_t *c, const char *confdir)
{
    if (c->conf_dir)
        free (c->conf_dir);
    c->conf_dir = xstrdup (confdir);
}

const char *flux_sec_get_directory (flux_sec_t *c)
{
    return c->conf_dir;
}

int flux_sec_disable (flux_sec_t *c, int tm)
{
    int rc;
    lock_sec (c);
    c->typemask &= ~tm;
    rc = validate_type (c->typemask);
    unlock_sec (c);
    return rc;
}

int flux_sec_enable (flux_sec_t *c, int tm)
{
    int rc;
    lock_sec (c);
    if ((tm & (FLUX_SEC_TYPE_CURVE))) /* plain/curve are like radio buttons */
        c->typemask &= ~(int)FLUX_SEC_TYPE_PLAIN;
    else if ((tm & (FLUX_SEC_TYPE_PLAIN)))
        c->typemask &= ~(int)FLUX_SEC_TYPE_CURVE;
    c->typemask |= tm;
    rc = validate_type (c->typemask);
    unlock_sec (c);
    return rc;
}

bool flux_sec_type_enabled (flux_sec_t *c, int tm)
{
    bool ret;
    lock_sec (c);
    ret = ((c->typemask & tm) == tm);
    unlock_sec (c);
    return ret;
}

int flux_sec_keygen (flux_sec_t *c, bool force, bool verbose)
{
    int rc = -1;
    lock_sec (c);
    if (checksecdirs (c, true) < 0)
        goto done_unlock;
    if ((c->typemask & FLUX_SEC_TYPE_CURVE)) {
        if (gencurve (c, "client", force, verbose) < 0)
            goto done_unlock;
        if (gencurve (c, "server", force, verbose) < 0)
            goto done_unlock;
    }
    if ((c->typemask & FLUX_SEC_TYPE_PLAIN)) {
        if (genpasswd (c, "client", force, verbose) < 0)
            goto done_unlock;
    }
    rc = 0;
done_unlock:
    unlock_sec (c);
    return rc;
}

int flux_sec_zauth_init (flux_sec_t *c, zctx_t *zctx, const char *domain)
{
    int rc = -1;
    lock_sec (c);
    if (checksecdirs (c, false) < 0)
        goto done_unlock;
    c->zctx = zctx;
    c->domain = xstrdup (domain ? domain : DEFAULT_ZAP_DOMAIN) ;
    if ((c->typemask & FLUX_SEC_TYPE_CURVE)) {
        if (!(c->zauth = zauth_new (c->zctx)))
            goto done_unlock;
        //zauth_set_verbose (c->zauth, true);
        if (!(c->cli_cert = getcurve (c, "client")))
            goto done_unlock;
        if (!(c->srv_cert = getcurve (c, "server")))
            goto done_unlock;
        zauth_configure_curve (c->zauth, "*", c->curve_dir);
    } else if ((c->typemask & FLUX_SEC_TYPE_PLAIN)) {
        if (!(c->zauth = zauth_new (c->zctx)))
            goto done_unlock;
        //zauth_set_verbose (c->zauth, true); // displays passwords on stdout!
        zauth_configure_plain (c->zauth, "*", c->passwd_file);
    }
    rc = 0;
done_unlock:
    unlock_sec (c);
    return rc;
}

int flux_sec_munge_init (flux_sec_t *c)
{
    int rc = -1;
    lock_sec (c);
    if ((c->typemask & FLUX_SEC_TYPE_MUNGE)
                            && !(c->typemask & FLUX_SEC_TYPE_FAKEMUNGE)) {
        munge_err_t e;
        if (!(c->mctx = munge_ctx_create ()))
            oom ();
        e = munge_ctx_set (c->mctx, MUNGE_OPT_UID_RESTRICTION, c->uid);
        if (e != EMUNGE_SUCCESS) {
            seterrstr (c, "munge_ctx_set: %s", munge_strerror (e));
            errno = EINVAL;
            goto done_unlock;
        }
    }
    rc = 0;
done_unlock:
    unlock_sec (c);
    return rc;
}

int flux_sec_csockinit (flux_sec_t *c, void *sock)
{
    int rc = -1;
    lock_sec (c);
    if ((c->typemask & FLUX_SEC_TYPE_CURVE)) {
        zsocket_set_zap_domain (sock, c->domain);
        zcert_apply (c->cli_cert, sock);
        zsocket_set_curve_serverkey (sock, zcert_public_txt (c->srv_cert));
    } else if ((c->typemask & FLUX_SEC_TYPE_PLAIN)) {
        char *passwd = NULL;
        if (!(passwd = getpasswd (c, "client"))) {
            seterrstr (c, "client not found in %s", c->passwd_file);
            goto done_unlock;
        }
        zsocket_set_plain_username (sock, "client");
        //zsocket_set_plain_password (sock, "treefrog"); // hah hah
        zsocket_set_plain_password (sock, passwd);
        free (passwd);
    }
    rc = 0;
done_unlock:
    unlock_sec (c);
    return rc;
}

int flux_sec_ssockinit (flux_sec_t *c, void *sock)
{
    lock_sec (c);
    if ((c->typemask & (FLUX_SEC_TYPE_CURVE))) {
        zsocket_set_zap_domain (sock, DEFAULT_ZAP_DOMAIN);
        zcert_apply (c->srv_cert, sock);
        zsocket_set_curve_server (sock, 1);
    } else if ((c->typemask & (FLUX_SEC_TYPE_PLAIN))) {
        zsocket_set_plain_server (sock, 1);
    }
    unlock_sec (c);
    return 0;
}

static int checksecdir (flux_sec_t *c, const char *path, bool create)
{
    struct stat sb;
    int rc = -1;

    /* XXX c->lock held */

    if (create && mkdir (path, 0700) < 0) {
        if (errno != EEXIST) {
            seterrstr (c, "mkdir %s: %s", path, strerror (errno));
            goto done;
        }
    }
    if (lstat (path, &sb) < 0) {
        if (errno == ENOENT) {
            seterrstr (c, "The directory '%s' does not exist. Have you run `flux keygen`?", path);
        }else{
            seterrstr (c, "lstat %s: %s", path, strerror (errno));
        }
        goto done;
    }
    if (!S_ISDIR (sb.st_mode)) {
        errno = ENOTDIR;
        seterrstr (c, "%s: %s", path, strerror (errno));
        goto done;
    }
    if ((sb.st_mode & (S_IRWXU|S_IRWXG|S_IRWXO)) != 0700) {
        seterrstr (c, "%s: mode should be 0700", path);
        errno = EPERM;
        goto done;
    }
    if ((sb.st_uid != c->uid)) {
        seterrstr (c, "%s: owner should be you", path);
        errno = EPERM;
        goto done;
    }
    rc = 0;
done:
    return rc;
}

static int checksecdirs (flux_sec_t *c, bool create)
{
    /* XXX c->lock held */

    if (!c->conf_dir) {
        seterrstr (c, "config directory is not set");
        errno = EINVAL;
        return -1;
    }
    if (!c->curve_dir) {
        if (asprintf (&c->curve_dir, "%s/curve", c->conf_dir) < 0)
            oom ();
    }
    if (!c->passwd_file) {
        if (asprintf (&c->passwd_file, "%s/passwd", c->conf_dir) < 0)
            oom ();
    }
    if (checksecdir (c, c->conf_dir, create) < 0)
        return -1;
    if (checksecdir (c, c->curve_dir, create) < 0)
        return -1;
    return 0;
}

static char * ctime_iso8601_now (char *buf, size_t sz)
{
    struct tm tm;
    time_t now = time (NULL);

    memset (buf, 0, sz);

    if (!localtime_r (&now, &tm))
        return (NULL);
    strftime (buf, sz, "%FT%T", &tm);

    return (buf);
}

static zcert_t *zcert_curve_new (flux_sec_t *c)
{
    zcert_t *new;
    char sec[41];
    char pub[41];
    uint8_t s[32];
    uint8_t p[32];

    if (zmq_curve_keypair (pub, sec) < 0) {
        if (errno == ENOTSUP)
            seterrstr (c,
                "No CURVE support in libzmq (not compiled with libsodium?)");
        else
            seterrstr (c,
                "Unknown error generating CURVE keypair");
        return NULL;
    }

    if (!zmq_z85_decode (s, sec) || !zmq_z85_decode (p, pub)) {
        seterrstr (c, "zcert_curve_new: Failed to decode keys");
        return NULL;
    }

    if (!(new = zcert_new_from (p, s)))
        oom ();

    return new;
}

static int gencurve (flux_sec_t *c, const char *role, bool force, bool verbose)
{
    char *path = NULL, *priv = NULL;;
    zcert_t *cert = NULL;
    char buf[64];
    struct stat sb;
    int rc = -1;

    /* XXX c->lock held */

    if (asprintf (&path, "%s/%s", c->curve_dir, role) < 0)
        oom ();
    if (asprintf (&priv, "%s/%s_private", c->curve_dir, role) < 0)
        oom ();
    if (force) {
        (void)unlink (path);
        (void)unlink (priv);
    }
    if (stat (path, &sb) == 0) {
        seterrstr (c, "%s exists, try --force", path);
        errno = EEXIST;
        goto done;
    }
    if (stat (priv, &sb) == 0) {
        seterrstr (c, "%s exists, try --force", priv);
        errno = EEXIST;
        goto done;
    }

    if (!(cert = zcert_curve_new (c)))
        goto done; /* error message set in zcert_curve_new() */

    zcert_set_meta (cert, "time", "%s", ctime_iso8601_now (buf, sizeof (buf)));
    zcert_set_meta (cert, "role", (char *)role);
    if (verbose) {
        printf ("Saving %s\n", path);
        printf ("Saving %s\n", priv);
    }
    if (zcert_save (cert, path) < 0) {
        seterrstr (c, "zcert_save %s: %s", path, strerror (errno));
        goto done;
    }
    rc = 0;
done:
    if (cert)
        zcert_destroy (&cert);
    if (path)
        free (path);
    if (priv)
        free (priv);
    return rc;
}

static zcert_t *getcurve (flux_sec_t *c, const char *role)
{
    char *path = NULL;;
    zcert_t *cert = NULL;

    /* XXX c->lock held */

    if (asprintf (&path, "%s/%s", c->curve_dir, role) < 0)
        oom ();
    if (!(cert = zcert_load (path)))
        seterrstr (c, "zcert_load %s: %s", path, flux_strerror (errno));
    free (path);
    return cert;
}

static char *getpasswd (flux_sec_t *c, const char *user)
{
    zhash_t *passwds;
    char *passwd = NULL;

    /* XXX c->lock held */

    if (!(passwds = zhash_new ()))
        oom ();
    zhash_autofree (passwds);
    if (zhash_load (passwds, c->passwd_file) < 0)
        goto done;
    if (!(passwd = zhash_lookup (passwds, user)))
        goto done;
    passwd = xstrdup (passwd);
done:
    if (passwds)
        zhash_destroy (&passwds);
    return passwd;
}

static int genpasswd (flux_sec_t *c, const char *user, bool force, bool verbose)
{
    struct stat sb;
    zhash_t *passwds = NULL;
    zuuid_t *uuid;
    mode_t old_mask;
    int rc = -1;

    /* XXX c->lock held */

    if (!(uuid = zuuid_new ()))
        oom ();
    if (force)
        (void)unlink (c->passwd_file);
    if (stat (c->passwd_file, &sb) == 0) {
        seterrstr (c, "%s exists, try --force", c->passwd_file);
        goto done;
    }
    if (!(passwds = zhash_new ()))
        oom ();
    zhash_update (passwds, user, (char *)zuuid_str (uuid));
    if (verbose)
        printf ("Saving %s\n", c->passwd_file);
    old_mask = umask (077);
    rc = zhash_save (passwds, c->passwd_file);
    umask (old_mask);
    if (rc < 0) {
        seterrstr (c, "zhash_save %s: %s", c->passwd_file, flux_strerror (errno));
        goto done;
    }
    /* FIXME: check created file mode */
    rc = 0;
done:
    if (passwds)
        zhash_destroy (&passwds);
    if (uuid)
        zuuid_destroy (&uuid);
    return rc;
}

int flux_sec_munge (flux_sec_t *c, const char *inbuf, size_t insize,
                    char **outbuf, size_t *outsize)
{
    munge_err_t e;
    int rc = -1;

    if (!inbuf || !outbuf || !outsize || !c
                          || !(c->typemask & FLUX_SEC_TYPE_MUNGE)) {
        errno = EINVAL;
        return -1;
    }
    lock_sec (c);
    if ((c->typemask & FLUX_SEC_TYPE_FAKEMUNGE)) {
        int dlen = base64_encode_length (insize);
        void *dst = xzmalloc (dlen);
        base64_encode_block (dst, &dlen, inbuf, insize);
        *outbuf = dst;
        *outsize = dlen;
    } else {
        if ((e = munge_encode (outbuf, c->mctx, inbuf,
                                                insize)) != EMUNGE_SUCCESS) {
            seterrstr (c, "munge_encode: %s", munge_strerror (e));
            errno = EKEYREJECTED;
            goto done_unlock;
        }
        *outsize = strlen (*outbuf) + 1; /* munge_decode needs null term */
    }
    rc = 0;
done_unlock:
    unlock_sec (c);
    return rc;
}

int flux_sec_unmunge (flux_sec_t *c, const char *inbuf, size_t insize,
                      char **outbuf, size_t *outsize)
{
    munge_err_t e;
    int rc = -1;

    if (!c || !inbuf || !outbuf || !outsize
                     || !(c->typemask & FLUX_SEC_TYPE_MUNGE)) {
        errno = EINVAL;
        return -1;
    }
    lock_sec (c);
    if ((c->typemask & FLUX_SEC_TYPE_FAKEMUNGE)) {
        int dlen = base64_decode_length (insize);
        void *dst = xzmalloc (dlen);
        if (base64_decode_block (dst, &dlen, inbuf, insize) < 0) {
            seterrstr (c, "munge_decode (fake) failed");
            free (dst);
            errno = EKEYREJECTED;
            goto done_unlock;
        }
        *outbuf = dst;
        *outsize = dlen;
    } else {
        if (inbuf[insize - 1] != '\0') {
            seterrstr (c, "munge cred is not null terminated");
            errno = EKEYREJECTED;
            goto done_unlock;
        }
        e = munge_decode (inbuf, c->mctx, (void **)outbuf, (int *)outsize,
                          NULL, NULL);
        if (e != EMUNGE_SUCCESS) {
            seterrstr (c, "munge_decode: %s", munge_strerror (e));
            errno = EKEYREJECTED;
            goto done_unlock;
        }
    }
    rc = 0;
done_unlock:
    unlock_sec (c);
    return rc;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
