/*
 * Copyright (c) 2000-2005, 2007-2015
 *	Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_STRING_H
# include <string.h>
#endif /* HAVE_STRING_H */
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif /* HAVE_STRINGS_H */
#include <unistd.h>
#if defined(HAVE_STDINT_H)
# include <stdint.h>
#elif defined(HAVE_INTTYPES_H)
# include <inttypes.h>
#endif
#ifdef HAVE_LOGIN_CAP_H
# include <login_cap.h>
# ifndef LOGIN_SETENV
#  define LOGIN_SETENV	0
# endif
#endif /* HAVE_LOGIN_CAP_H */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>

#include "sudoers.h"

/*
 * Flags used in rebuild_env()
 */
#undef DID_TERM
#define DID_TERM	0x0001
#undef DID_PATH
#define DID_PATH	0x0002
#undef DID_HOME
#define DID_HOME	0x0004
#undef DID_SHELL
#define DID_SHELL	0x0008
#undef DID_LOGNAME
#define DID_LOGNAME	0x0010
#undef DID_USER
#define DID_USER    	0x0020
#undef DID_USERNAME
#define DID_USERNAME   	0x0040
#undef DID_MAIL
#define DID_MAIL   	0x0080
#undef DID_MAX
#define DID_MAX    	0x00ff

#undef KEPT_TERM
#define KEPT_TERM	0x0100
#undef KEPT_PATH
#define KEPT_PATH	0x0200
#undef KEPT_HOME
#define KEPT_HOME	0x0400
#undef KEPT_SHELL
#define KEPT_SHELL	0x0800
#undef KEPT_LOGNAME
#define KEPT_LOGNAME	0x1000
#undef KEPT_USER
#define KEPT_USER    	0x2000
#undef KEPT_USERNAME
#define KEPT_USERNAME	0x4000
#undef KEPT_MAIL
#define KEPT_MAIL	0x8000
#undef KEPT_MAX
#define KEPT_MAX    	0xff00

struct environment {
    char **envp;		/* pointer to the new environment */
    char **old_envp;		/* pointer the old environment we allocated */
    size_t env_size;		/* size of new_environ in char **'s */
    size_t env_len;		/* number of slots used, not counting NULL */
};

/*
 * Copy of the sudo-managed environment.
 */
static struct environment env;

/*
 * Default table of "bad" variables to remove from the environment.
 * XXX - how to omit TERMCAP if it starts with '/'?
 */
static const char *initial_badenv_table[] = {
    "IFS",
    "CDPATH",
    "LOCALDOMAIN",
    "RES_OPTIONS",
    "HOSTALIASES",
    "NLSPATH",
    "PATH_LOCALE",
    "LD_*",
    "_RLD*",
#ifdef __hpux
    "SHLIB_PATH",
#endif /* __hpux */
#ifdef _AIX
    "LDR_*",
    "LIBPATH",
    "AUTHSTATE",
#endif
#ifdef __APPLE__
    "DYLD_*",
#endif
#ifdef HAVE_KERB5
    "KRB5_CONFIG*",
    "KRB5_KTNAME",
#endif /* HAVE_KERB5 */
#ifdef HAVE_SECURID
    "VAR_ACE",
    "USR_ACE",
    "DLC_ACE",
#endif /* HAVE_SECURID */
    "TERMINFO",			/* terminfo, exclusive path to terminfo files */
    "TERMINFO_DIRS",		/* terminfo, path(s) to terminfo files */
    "TERMPATH",			/* termcap, path(s) to termcap files */
    "TERMCAP",			/* XXX - only if it starts with '/' */
    "ENV",			/* ksh, file to source before script runs */
    "BASH_ENV",			/* bash, file to source before script runs */
    "PS4",			/* bash, prefix for lines in xtrace mode */
    "GLOBIGNORE",		/* bash, globbing patterns to ignore */
    "BASHOPTS",			/* bash, initial "shopt -s" options */
    "SHELLOPTS",		/* bash, initial "set -o" options */
    "JAVA_TOOL_OPTIONS",	/* java, extra command line options */
    "PERLIO_DEBUG ",		/* perl, debugging output file */
    "PERLLIB",			/* perl, search path for modules/includes */
    "PERL5LIB",			/* perl 5, search path for modules/includes */
    "PERL5OPT",			/* perl 5, extra command line options */
    "PERL5DB",			/* perl 5, command used to load debugger */
    "FPATH",			/* ksh, search path for functions */
    "NULLCMD",			/* zsh, command for null file redirection */
    "READNULLCMD",		/* zsh, command for null file redirection */
    "ZDOTDIR",			/* zsh, search path for dot files */
    "TMPPREFIX",		/* zsh, prefix for temporary files */
    "PYTHONHOME",		/* python, module search path */
    "PYTHONPATH",		/* python, search path */
    "PYTHONINSPECT",		/* python, allow inspection */
    "PYTHONUSERBASE",		/* python, per user site-packages directory */
    "RUBYLIB",			/* ruby, library load path */
    "RUBYOPT",			/* ruby, extra command line options */
    "BASH_FUNC_*",		/* new-style bash functions */
    "__BASH_FUNC<*",		/* new-style bash functions (Apple) */
    NULL
};

/*
 * Default table of variables to check for '%' and '/' characters.
 */
static const char *initial_checkenv_table[] = {
    "COLORTERM",
    "LANG",
    "LANGUAGE",
    "LC_*",
    "LINGUAS",
    "TERM",
    "TZ",
    NULL
};

/*
 * Default table of variables to preserve in the environment.
 */
static const char *initial_keepenv_table[] = {
    "COLORS",
    "DISPLAY",
    "HOSTNAME",
    "KRB5CCNAME",
    "LS_COLORS",
    "PATH",
    "PS1",
    "PS2",
    "XAUTHORITY",
    "XAUTHORIZATION",
    NULL
};

/*
 * Initialize env based on envp.
 */
bool
env_init(char * const envp[])
{
    char * const *ep;
    size_t len;
    debug_decl(env_init, SUDOERS_DEBUG_ENV)

    if (envp == NULL) {
	/* Free the old envp we allocated, if any. */
	free(env.old_envp);

	/* Reset to initial state but keep a pointer to what we allocated. */
	env.old_envp = env.envp;
	env.envp = NULL;
	env.env_size = 0;
	env.env_len = 0;
    } else {
	/* Make private copy of envp. */
	for (ep = envp; *ep != NULL; ep++)
	    continue;
	len = (size_t)(ep - envp);

	env.env_len = len;
	env.env_size = len + 1 + 128;
	env.envp = reallocarray(NULL, env.env_size, sizeof(char *));
	if (env.envp == NULL) {
	    env.env_size = 0;
	    env.env_len = 0;
	    sudo_warnx(U_("%s: %s"), __func__, U_("unable to allocate memory"));
	    debug_return_bool(false);
	}
#ifdef ENV_DEBUG
	memset(env.envp, 0, env.env_size * sizeof(char *));
#endif
	memcpy(env.envp, envp, len * sizeof(char *));
	env.envp[len] = NULL;

	/* Free the old envp we allocated, if any. */
	free(env.old_envp);
	env.old_envp = NULL;
    }

    debug_return_bool(true);
}

/*
 * Getter for private copy of the environment.
 */
char **
env_get(void)
{
    return env.envp;
}

/*
 * Swap the old and new copies of the environment.
 */
bool
env_swap_old(void)
{
    char **old_envp;

    if (env.old_envp == NULL)
	return false;
    old_envp = env.old_envp;
    env.old_envp = env.envp;
    env.envp = old_envp;
    return true;
}

/*
 * Similar to putenv(3) but operates on sudo's private copy of the
 * environment (not environ) and it always overwrites.  The dupcheck param
 * determines whether we need to verify that the variable is not already set.
 * Will only overwrite an existing variable if overwrite is set.
 * Does not include warnings or debugging to avoid recursive calls.
 */
static int
sudo_putenv_nodebug(char *str, bool dupcheck, bool overwrite)
{
    char **ep;
    size_t len;
    bool found = false;

    /* Make sure there is room for the new entry plus a NULL. */
    if (env.env_size > 2 && env.env_len > env.env_size - 2) {
	char **nenvp;
	size_t nsize;

	if (env.env_size > SIZE_MAX - 128) {
	    sudo_warnx_nodebug(U_("internal error, %s overflow"),
		"sudo_putenv_nodebug");
	    errno = EOVERFLOW;
	    return -1;
	}
	nsize = env.env_size + 128;
	if (nsize > SIZE_MAX / sizeof(char *)) {
	    sudo_warnx_nodebug(U_("internal error, %s overflow"),
		"sudo_putenv_nodebug");
	    errno = EOVERFLOW;
	    return -1;
	}
	nenvp = reallocarray(env.envp, nsize, sizeof(char *));
	if (nenvp == NULL)
	    return -1;
	env.envp = nenvp;
	env.env_size = nsize;
#ifdef ENV_DEBUG
	memset(env.envp + env.env_len, 0,
	    (env.env_size - env.env_len) * sizeof(char *));
#endif
    }

#ifdef ENV_DEBUG
    if (env.envp[env.env_len] != NULL) {
	errno = EINVAL;
	return -1;
    }
#endif

    if (dupcheck) {
	len = (strchr(str, '=') - str) + 1;
	for (ep = env.envp; *ep != NULL; ep++) {
	    if (strncmp(str, *ep, len) == 0) {
		if (overwrite)
		    *ep = str;
		found = true;
		break;
	    }
	}
	/* Prune out extra instances of the variable we just overwrote. */
	if (found && overwrite) {
	    while (*++ep != NULL) {
		if (strncmp(str, *ep, len) == 0) {
		    char **cur = ep;
		    while ((*cur = *(cur + 1)) != NULL)
			cur++;
		    ep--;
		}
	    }
	    env.env_len = ep - env.envp;
	}
    }

    if (!found) {
	ep = env.envp + env.env_len;
	env.env_len++;
	*ep++ = str;
	*ep = NULL;
    }
    return 0;
}

/*
 * Similar to putenv(3) but operates on sudo's private copy of the
 * environment (not environ) and it always overwrites.  The dupcheck param
 * determines whether we need to verify that the variable is not already set.
 * Will only overwrite an existing variable if overwrite is set.
 */
static int
sudo_putenv(char *str, bool dupcheck, bool overwrite)
{
    int rval;
    debug_decl(sudo_putenv, SUDOERS_DEBUG_ENV)

    sudo_debug_printf(SUDO_DEBUG_INFO, "sudo_putenv: %s", str);

    rval = sudo_putenv_nodebug(str, dupcheck, overwrite);
    if (rval == -1) {
#ifdef ENV_DEBUG
	if (env.envp[env.env_len] != NULL)
	    sudo_warnx(U_("sudo_putenv: corrupted envp, length mismatch"));
#endif
    }
    debug_return_int(rval);
}

/*
 * Similar to setenv(3) but operates on a private copy of the environment.
 * The dupcheck param determines whether we need to verify that the variable
 * is not already set.
 */
static int
sudo_setenv2(const char *var, const char *val, bool dupcheck, bool overwrite)
{
    char *estring;
    size_t esize;
    int rval = -1;
    debug_decl(sudo_setenv2, SUDOERS_DEBUG_ENV)

    esize = strlen(var) + 1 + strlen(val) + 1;
    if ((estring = malloc(esize)) == NULL) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "unable to allocate memory");
	debug_return_int(-1);
    }

    /* Build environment string and insert it. */
    if (strlcpy(estring, var, esize) >= esize ||
	strlcat(estring, "=", esize) >= esize ||
	strlcat(estring, val, esize) >= esize) {

	sudo_warnx(U_("internal error, %s overflow"), __func__);
	errno = EOVERFLOW;
    } else {
	rval = sudo_putenv(estring, dupcheck, overwrite);
    }
    if (rval == -1)
	free(estring);
    debug_return_int(rval);
}

/*
 * Similar to setenv(3) but operates on a private copy of the environment.
 */
int
sudo_setenv(const char *var, const char *val, int overwrite)
{
    return sudo_setenv2(var, val, true, (bool)overwrite);
}

/*
 * Similar to setenv(3) but operates on a private copy of the environment.
 * Does not include warnings or debugging to avoid recursive calls.
 */
static int
sudo_setenv_nodebug(const char *var, const char *val, int overwrite)
{
    char *ep, *estring = NULL;
    const char *cp;
    size_t esize;
    int rval = -1;

    if (var == NULL || *var == '\0') {
	errno = EINVAL;
	goto done;
    }

    /*
     * POSIX says a var name with '=' is an error but BSD
     * just ignores the '=' and anything after it.
     */
    for (cp = var; *cp && *cp != '='; cp++)
	;
    esize = (size_t)(cp - var) + 2;
    if (val) {
	esize += strlen(val);	/* glibc treats a NULL val as "" */
    }

    /* Allocate and fill in estring. */
    if ((estring = ep = malloc(esize)) == NULL)
	goto done;
    for (cp = var; *cp && *cp != '='; cp++)
	*ep++ = *cp;
    *ep++ = '=';
    if (val) {
	for (cp = val; *cp; cp++)
	    *ep++ = *cp;
    }
    *ep = '\0';

    rval = sudo_putenv_nodebug(estring, true, overwrite);
done:
    if (rval == -1)
	free(estring);
    return rval;
}

/*
 * Similar to unsetenv(3) but operates on a private copy of the environment.
 * Does not include warnings or debugging to avoid recursive calls.
 */
static int
sudo_unsetenv_nodebug(const char *var)
{
    char **ep = env.envp;
    size_t len;

    if (ep == NULL || var == NULL || *var == '\0' || strchr(var, '=') != NULL) {
	errno = EINVAL;
	return -1;
    }

    len = strlen(var);
    while (*ep != NULL) {
	if (strncmp(var, *ep, len) == 0 && (*ep)[len] == '=') {
	    /* Found it; shift remainder + NULL over by one. */
	    char **cur = ep;
	    while ((*cur = *(cur + 1)) != NULL)
		cur++;
	    /* Keep going, could be multiple instances of the var. */
	} else {
	    ep++;
	}
    }
    return 0;
}

/*
 * Similar to unsetenv(3) but operates on a private copy of the environment.
 */
int
sudo_unsetenv(const char *name)
{
    int rval;
    debug_decl(sudo_unsetenv, SUDOERS_DEBUG_ENV)

    sudo_debug_printf(SUDO_DEBUG_INFO, "sudo_unsetenv: %s", name);

    rval = sudo_unsetenv_nodebug(name);

    debug_return_int(rval);
}

/*
 * Similar to getenv(3) but operates on a private copy of the environment.
 * Does not include warnings or debugging to avoid recursive calls.
 */
static char *
sudo_getenv_nodebug(const char *name)
{
    char **ep, *val = NULL;
    size_t namelen = 0;

    if (env.env_len != 0) {
	/* For BSD compatibility, treat '=' in name like end of string. */
	while (name[namelen] != '\0' && name[namelen] != '=')
	    namelen++;
	for (ep = env.envp; *ep != NULL; ep++) {
	    if (strncmp(*ep, name, namelen) == 0 && (*ep)[namelen] == '=') {
		val = *ep + namelen + 1;
		break;
	    }
	}
    }
    return val;
}

/*
 * Similar to getenv(3) but operates on a private copy of the environment.
 */
char *
sudo_getenv(const char *name)
{
    char *val;
    debug_decl(sudo_getenv, SUDOERS_DEBUG_ENV)

    sudo_debug_printf(SUDO_DEBUG_INFO, "sudo_getenv: %s", name);

    val = sudo_getenv_nodebug(name);

    debug_return_str(val);
}

/*
 * Check for var against patterns in the specified environment list.
 * Returns true if the variable was found, else false.
 */
static bool
matches_env_list(const char *var, struct list_members *list, bool *full_match)
{
    struct list_member *cur;
    bool match = false;
    debug_decl(matches_env_list, SUDOERS_DEBUG_ENV)

    SLIST_FOREACH(cur, list, entries) {
	size_t sep_pos, len = strlen(cur->value);
	bool iswild = false;

	/* Locate position of the '=' separator in var=value. */
	sep_pos = strcspn(var, "=");

	/* Deal with '*' wildcard at the end of the pattern. */
	if (cur->value[len - 1] == '*') {
	    len--;
	    iswild = true;
	}
	if (strncmp(cur->value, var, len) == 0 &&
	    (iswild || len == sep_pos || var[len] == '\0')) {
	    /* If we matched past the '=', count as a full match. */
	    *full_match = len > sep_pos + 1;
	    match = true;
	    break;
	}
    }
    debug_return_bool(match);
}

/*
 * Check the env_delete blacklist.
 * Returns true if the variable was found, else false.
 */
static bool
matches_env_delete(const char *var)
{
    bool full_match;	/* unused */
    debug_decl(matches_env_delete, SUDOERS_DEBUG_ENV)

    /* Skip anything listed in env_delete. */
    debug_return_bool(matches_env_list(var, &def_env_delete, &full_match));
}

/*
 * Sanity-check the TZ environment variable.
 * On many systems it is possible to set this to a pathname.
 */
static bool
tz_is_sane(const char *tzval)
{
    const char *cp;
    char lastch;
    debug_decl(tz_is_sane, SUDOERS_DEBUG_ENV)

    /* tzcode treats a value beginning with a ':' as a path. */
    if (tzval[0] == ':')
	tzval++;

    /* Reject fully-qualified TZ that doesn't being with the zoneinfo dir. */
    if (tzval[0] == '/') {
#ifdef _PATH_ZONEINFO
	if (strncmp(tzval, _PATH_ZONEINFO, sizeof(_PATH_ZONEINFO) - 1) != 0 ||
	    tzval[sizeof(_PATH_ZONEINFO) - 1] != '/')
	    debug_return_bool(false);
#else
	/* Assume the worst. */
	debug_return_bool(false);
#endif
    }

    /*
     * Make sure TZ only contains printable non-space characters
     * and does not contain a '..' path element.
     */
    lastch = '/';
    for (cp = tzval; *cp != '\0'; cp++) {
	if (isspace((unsigned char)*cp) || !isprint((unsigned char)*cp))
	    debug_return_bool(false);
	if (lastch == '/' && cp[0] == '.' && cp[1] == '.' &&
	    (cp[2] == '/' || cp[2] == '\0'))
	    debug_return_bool(false);
	lastch = *cp;
    }

    /* Reject extra long TZ values (even if not a path). */
    if ((size_t)(cp - tzval) >= PATH_MAX)
	debug_return_bool(false);

    debug_return_bool(true);
}

/*
 * Apply the env_check list.
 * Returns true if the variable is allowed, false if denied
 * or -1 if no match.
 */
static int
matches_env_check(const char *var, bool *full_match)
{
    int keepit = -1;
    debug_decl(matches_env_check, SUDOERS_DEBUG_ENV)

    /* Skip anything listed in env_check that includes '/' or '%'. */
    if (matches_env_list(var, &def_env_check, full_match)) {
	if (strncmp(var, "TZ=", 3) == 0) {
	    /* Special case for TZ */
	    keepit = tz_is_sane(var + 3);
	} else {
	    const char *val = strchr(var, '=');
	    if (val != NULL)
		keepit = !strpbrk(++val, "/%");
	}
    }
    debug_return_int(keepit);
}

/*
 * Check the env_keep list.
 * Returns true if the variable is allowed else false.
 */
static bool
matches_env_keep(const char *var, bool *full_match)
{
    bool keepit = false;
    debug_decl(matches_env_keep, SUDOERS_DEBUG_ENV)

    /* Preserve SHELL variable for "sudo -s". */
    if (ISSET(sudo_mode, MODE_SHELL) && strncmp(var, "SHELL=", 6) == 0) {
	keepit = true;
    } else if (matches_env_list(var, &def_env_keep, full_match)) {
	keepit = true;
    }
    debug_return_bool(keepit);
}

/*
 * Look up var in the env_delete and env_check.
 * Returns true if we should delete the variable, else false.
 */
static bool
env_should_delete(const char *var)
{
    const char *cp;
    int delete_it;
    bool full_match = false;
    debug_decl(env_should_delete, SUDOERS_DEBUG_ENV);

    /* Skip variables with values beginning with () (bash functions) */
    if ((cp = strchr(var, '=')) != NULL) {
	if (strncmp(cp, "=() ", 3) == 0) {
	    delete_it = true;
	    goto done;
	}
    }

    delete_it = matches_env_delete(var);
    if (!delete_it)
	delete_it = matches_env_check(var, &full_match) == false;

done:
    sudo_debug_printf(SUDO_DEBUG_INFO, "delete %s: %s",
	var, delete_it ? "YES" : "NO");
    debug_return_bool(delete_it);
}

/*
 * Lookup var in the env_check and env_keep lists.
 * Returns true if the variable is allowed else false.
 */
static bool
env_should_keep(const char *var)
{
    int keepit;
    bool full_match = false;
    const char *cp;
    debug_decl(env_should_keep, SUDOERS_DEBUG_ENV)

    keepit = matches_env_check(var, &full_match);
    if (keepit == -1)
	keepit = matches_env_keep(var, &full_match);

    /* Skip bash functions unless we matched on the value as well as name. */
    if (keepit && !full_match) {
	if ((cp = strchr(var, '=')) != NULL) {
	    if (strncmp(cp, "=() ", 3) == 0)
		keepit = false;
	}
    }
    sudo_debug_printf(SUDO_DEBUG_INFO, "keep %s: %s",
	var, keepit == true ? "YES" : "NO");
    debug_return_bool(keepit == true);
}

#ifdef HAVE_PAM
/*
 * Merge another environment with our private copy.
 * Only overwrite an existing variable if it is not
 * being preserved from the user's environment.
 * Returns true on success or false on failure.
 */
bool
env_merge(char * const envp[])
{
    char * const *ep;
    bool rval = true;
    debug_decl(env_merge, SUDOERS_DEBUG_ENV)

    for (ep = envp; *ep != NULL; ep++) {
	/* XXX - avoid checking value here, should only check name */
	bool overwrite = def_env_reset ? !env_should_keep(*ep) : env_should_delete(*ep);
	if (sudo_putenv(*ep, true, overwrite) == -1) {
	    /* XXX cannot undo on failure */
	    rval = false;
	    break;
	}
    }
    debug_return_bool(rval);
}
#endif /* HAVE_PAM */

static void
env_update_didvar(const char *ep, unsigned int *didvar)
{
    switch (*ep) {
	case 'H':
	    if (strncmp(ep, "HOME=", 5) == 0)
		SET(*didvar, DID_HOME);
	    break;
	case 'L':
	    if (strncmp(ep, "LOGNAME=", 8) == 0)
		SET(*didvar, DID_LOGNAME);
	    break;
	case 'M':
	    if (strncmp(ep, "MAIL=", 5) == 0)
		SET(*didvar, DID_MAIL);
	    break;
	case 'P':
	    if (strncmp(ep, "PATH=", 5) == 0)
		SET(*didvar, DID_PATH);
	    break;
	case 'S':
	    if (strncmp(ep, "SHELL=", 6) == 0)
		SET(*didvar, DID_SHELL);
	    break;
	case 'T':
	    if (strncmp(ep, "TERM=", 5) == 0)
		SET(*didvar, DID_TERM);
	    break;
	case 'U':
	    if (strncmp(ep, "USER=", 5) == 0)
		SET(*didvar, DID_USER);
	    if (strncmp(ep, "USERNAME=", 5) == 0)
		SET(*didvar, DID_USERNAME);
	    break;
    }
}

#define CHECK_PUTENV(a, b, c)	do {					       \
    if (sudo_putenv((a), (b), (c)) == -1)				       \
	goto bad;							       \
} while (0)

#define CHECK_SETENV2(a, b, c, d)	do {				       \
    if (sudo_setenv2((a), (b), (c), (d)) == -1)				       \
	goto bad;							       \
} while (0)

/*
 * Build a new environment and ether clear potentially dangerous
 * variables from the old one or start with a clean slate.
 * Also adds sudo-specific variables (SUDO_*).
 * Returns true on success or false on failure.
 */
bool
rebuild_env(void)
{
    char **ep, *cp, *ps1;
    char idbuf[MAX_UID_T_LEN + 1];
    unsigned int didvar;
    bool reset_home = false;
    debug_decl(rebuild_env, SUDOERS_DEBUG_ENV)

    /*
     * Either clean out the environment or reset to a safe default.
     */
    ps1 = NULL;
    didvar = 0;
    env.env_len = 0;
    env.env_size = 128;
    free(env.old_envp);
    env.old_envp = env.envp;
    env.envp = reallocarray(NULL, env.env_size, sizeof(char *));
    if (env.envp == NULL) {
	sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
	    "unable to allocate memory");
	env.env_size = 0;
	goto bad;
    }
#ifdef ENV_DEBUG
    memset(env.envp, 0, env.env_size * sizeof(char *));
#else
    env.envp[0] = NULL;
#endif

    /* Reset HOME based on target user if configured to. */
    if (ISSET(sudo_mode, MODE_RUN)) {
	if (def_always_set_home ||
	    ISSET(sudo_mode, MODE_RESET_HOME | MODE_LOGIN_SHELL) || 
	    (ISSET(sudo_mode, MODE_SHELL) && def_set_home))
	    reset_home = true;
    }

    if (def_env_reset || ISSET(sudo_mode, MODE_LOGIN_SHELL)) {
	/*
	 * If starting with a fresh environment, initialize it based on
	 * /etc/environment or login.conf.  For "sudo -i" we want those
	 * variables to override the invoking user's environment, so we
	 * defer reading them until later.
	 */
	if (!ISSET(sudo_mode, MODE_LOGIN_SHELL)) {
#ifdef HAVE_LOGIN_CAP_H
	    /* Insert login class environment variables. */
	    if (login_class) {
		login_cap_t *lc = login_getclass(login_class);
		if (lc != NULL) {
		    setusercontext(lc, runas_pw, runas_pw->pw_uid,
			LOGIN_SETPATH|LOGIN_SETENV);
		    login_close(lc);
		}
	    }
#endif /* HAVE_LOGIN_CAP_H */
#if defined(_AIX) || (defined(__linux__) && !defined(HAVE_PAM))
	    /* Insert system-wide environment variables. */
	    read_env_file(_PATH_ENVIRONMENT, true);
#endif
	    for (ep = env.envp; *ep; ep++)
		env_update_didvar(*ep, &didvar);
	}

	/* Pull in vars we want to keep from the old environment. */
	for (ep = env.old_envp; *ep; ep++) {
	    bool keepit;

	    /*
	     * Look up the variable in the env_check and env_keep lists.
	     */
	    keepit = env_should_keep(*ep);

	    /*
	     * Do SUDO_PS1 -> PS1 conversion.
	     * This must happen *after* env_should_keep() is called.
	     */
	    if (strncmp(*ep, "SUDO_PS1=", 8) == 0)
		ps1 = *ep + 5;

	    if (keepit) {
		/* Preserve variable. */
		CHECK_PUTENV(*ep, true, false);
		env_update_didvar(*ep, &didvar);
	    }
	}
	didvar |= didvar << 8;		/* convert DID_* to KEPT_* */

	/*
	 * Add in defaults.  In -i mode these come from the runas user,
	 * otherwise they may be from the user's environment (depends
	 * on sudoers options).
	 */
	if (ISSET(sudo_mode, MODE_LOGIN_SHELL)) {
	    CHECK_SETENV2("SHELL", runas_pw->pw_shell,
		ISSET(didvar, DID_SHELL), true);
	    CHECK_SETENV2("LOGNAME", runas_pw->pw_name,
		ISSET(didvar, DID_LOGNAME), true);
	    CHECK_SETENV2("USER", runas_pw->pw_name,
		ISSET(didvar, DID_USER), true);
	    CHECK_SETENV2("USERNAME", runas_pw->pw_name,
		ISSET(didvar, DID_USERNAME), true);
	} else {
	    /* We will set LOGNAME later in the def_set_logname case. */
	    if (!def_set_logname) {
		if (!ISSET(didvar, DID_LOGNAME))
		    CHECK_SETENV2("LOGNAME", user_name, false, true);
		if (!ISSET(didvar, DID_USER))
		    CHECK_SETENV2("USER", user_name, false, true);
		if (!ISSET(didvar, DID_USERNAME))
		    CHECK_SETENV2("USERNAME", user_name, false, true);
	    }
	}

	/* If we didn't keep HOME, reset it based on target user. */
	if (!ISSET(didvar, KEPT_HOME))
	    reset_home = true;

	/*
	 * Set MAIL to target user in -i mode or if MAIL is not preserved
	 * from user's environment.
	 */
	if (ISSET(sudo_mode, MODE_LOGIN_SHELL) || !ISSET(didvar, KEPT_MAIL)) {
	    cp = _PATH_MAILDIR;
	    if (cp[sizeof(_PATH_MAILDIR) - 2] == '/') {
		if (asprintf(&cp, "MAIL=%s%s", _PATH_MAILDIR, runas_pw->pw_name) == -1)
		    goto bad;
	    } else {
		if (asprintf(&cp, "MAIL=%s/%s", _PATH_MAILDIR, runas_pw->pw_name) == -1)
		    goto bad;
	    }
	    if (sudo_putenv(cp, ISSET(didvar, DID_MAIL), true) == -1) {
		free(cp);
		goto bad;
	    }
	}
    } else {
	/*
	 * Copy environ entries as long as they don't match env_delete or
	 * env_check.
	 */
	for (ep = env.old_envp; *ep; ep++) {
	    /* Add variable unless it matches a black list. */
	    if (!env_should_delete(*ep)) {
		if (strncmp(*ep, "SUDO_PS1=", 9) == 0)
		    ps1 = *ep + 5;
		else if (strncmp(*ep, "SHELL=", 6) == 0)
		    SET(didvar, DID_SHELL);
		else if (strncmp(*ep, "PATH=", 5) == 0)
		    SET(didvar, DID_PATH);
		else if (strncmp(*ep, "TERM=", 5) == 0)
		    SET(didvar, DID_TERM);
		CHECK_PUTENV(*ep, true, false);
	    }
	}
    }
    /* Replace the PATH envariable with a secure one? */
    if (def_secure_path && !user_is_exempt()) {
	CHECK_SETENV2("PATH", def_secure_path, true, true);
	SET(didvar, DID_PATH);
    }

    /*
     * Set $USER, $LOGNAME and $USERNAME to target if "set_logname" is not
     * disabled.  We skip this if we are running a login shell (because
     * they have already been set).
     */
    if (def_set_logname && !ISSET(sudo_mode, MODE_LOGIN_SHELL)) {
	if (!ISSET(didvar, (KEPT_LOGNAME|KEPT_USER|KEPT_USERNAME))) {
	    /* Nothing preserved, set all three. */
	    CHECK_SETENV2("LOGNAME", runas_pw->pw_name, true, true);
	    CHECK_SETENV2("USER", runas_pw->pw_name, true, true);
	    CHECK_SETENV2("USERNAME", runas_pw->pw_name, true, true);
	} else if ((didvar & (KEPT_LOGNAME|KEPT_USER|KEPT_USERNAME)) !=
	    (KEPT_LOGNAME|KEPT_USER|KEPT_USERNAME)) {
	    /*
	     * Preserved some of LOGNAME, USER, USERNAME but not all.
	     * Make the unset ones match so we don't end up with some
	     * set to the invoking user and others set to the runas user.
	     */
	    if (ISSET(didvar, KEPT_LOGNAME))
		cp = sudo_getenv("LOGNAME");
	    else if (ISSET(didvar, KEPT_USER))
		cp = sudo_getenv("USER");
	    else if (ISSET(didvar, KEPT_USERNAME))
		cp = sudo_getenv("USERNAME");
	    else
		cp = NULL;
	    if (cp != NULL) {
		if (!ISSET(didvar, KEPT_LOGNAME))
		    CHECK_SETENV2("LOGNAME", cp, true, true);
		if (!ISSET(didvar, KEPT_USER))
		    CHECK_SETENV2("USER", cp, true, true);
		if (!ISSET(didvar, KEPT_USERNAME))
		    CHECK_SETENV2("USERNAME", cp, true, true);
	    }
	}
    }

    /* Set $HOME to target user if not preserving user's value. */
    if (reset_home)
	CHECK_SETENV2("HOME", runas_pw->pw_dir, true, true);

    /* Provide default values for $SHELL, $TERM and $PATH if not set. */
    if (!ISSET(didvar, DID_SHELL))
	CHECK_SETENV2("SHELL", runas_pw->pw_shell, false, false);
    if (!ISSET(didvar, DID_TERM))
	CHECK_PUTENV("TERM=unknown", false, false);
    if (!ISSET(didvar, DID_PATH))
	CHECK_SETENV2("PATH", _PATH_STDPATH, false, true);

    /* Set PS1 if SUDO_PS1 is set. */
    if (ps1 != NULL)
	CHECK_PUTENV(ps1, true, true);

    /* Add the SUDO_COMMAND envariable (cmnd + args). */
    if (user_args) {
	if (asprintf(&cp, "SUDO_COMMAND=%s %s", user_cmnd, user_args) == -1)
	    goto bad;
	if (sudo_putenv(cp, true, true) == -1) {
	    free(cp);
	    goto bad;
	}
    } else {
	CHECK_SETENV2("SUDO_COMMAND", user_cmnd, true, true);
    }

    /* Add the SUDO_USER, SUDO_UID, SUDO_GID environment variables. */
    CHECK_SETENV2("SUDO_USER", user_name, true, true);
    snprintf(idbuf, sizeof(idbuf), "%u", (unsigned int) user_uid);
    CHECK_SETENV2("SUDO_UID", idbuf, true, true);
    snprintf(idbuf, sizeof(idbuf), "%u", (unsigned int) user_gid);
    CHECK_SETENV2("SUDO_GID", idbuf, true, true);

    debug_return_bool(true);

bad:
    sudo_warn(U_("unable to rebuild the environment"));
    debug_return_bool(false);
}

/*
 * Insert all environment variables in envp into the private copy
 * of the environment.
 * Returns true on success or false on failure.
 */
bool
insert_env_vars(char * const envp[])
{
    char * const *ep;
    bool rval = true;
    debug_decl(insert_env_vars, SUDOERS_DEBUG_ENV)

    /* Add user-specified environment variables. */
    if (envp != NULL) {
	for (ep = envp; *ep != NULL; ep++) {
	    /* XXX - no undo on failure */
	    if (sudo_putenv(*ep, true, true) == -1) {
		rval = false;
		break;
	    }
	}
    }
    debug_return_bool(rval);
}

/*
 * Validate the list of environment variables passed in on the command
 * line against env_delete, env_check, and env_keep.
 * Calls log_warning() if any specified variables are not allowed.
 * Returns true if allowed, else false.
 */
bool
validate_env_vars(char * const env_vars[])
{
    char * const *ep;
    char *eq, errbuf[4096];
    bool okvar, rval = true;
    debug_decl(validate_env_vars, SUDOERS_DEBUG_ENV)

    if (env_vars == NULL)
	debug_return_bool(true);	/* nothing to do */

    /* Add user-specified environment variables. */
    errbuf[0] = '\0';
    for (ep = env_vars; *ep != NULL; ep++) {
	if (def_secure_path && !user_is_exempt() &&
	    strncmp(*ep, "PATH=", 5) == 0) {
	    okvar = false;
	} else if (def_env_reset) {
	    okvar = env_should_keep(*ep);
	} else {
	    okvar = !env_should_delete(*ep);
	}
	if (okvar == false) {
	    /* Not allowed, add to error string, allocating as needed. */
	    if ((eq = strchr(*ep, '=')) != NULL)
		*eq = '\0';
	    if (errbuf[0] != '\0')
		(void)strlcat(errbuf, ", ", sizeof(errbuf));
	    if (strlcat(errbuf, *ep, sizeof(errbuf)) >= sizeof(errbuf)) {
		errbuf[sizeof(errbuf) - 4] = '\0';
		(void)strlcat(errbuf, "...", sizeof(errbuf));
	    }
	    if (eq != NULL)
		*eq = '=';
	}
    }
    if (errbuf[0] != '\0') {
	/* XXX - audit? */
	log_warningx(0,
	    N_("sorry, you are not allowed to set the following environment variables: %s"), errbuf);
	rval = false;
    }
    debug_return_bool(rval);
}

/*
 * Read in /etc/environment ala AIX and Linux.
 * Lines may be in either of three formats:
 *  NAME=VALUE
 *  NAME="VALUE"
 *  NAME='VALUE'
 * with an optional "export" prefix so the shell can source the file.
 * Invalid lines, blank lines, or lines consisting solely of a comment
 * character are skipped.
 */
bool
read_env_file(const char *path, int overwrite)
{
    FILE *fp;
    bool rval = true;
    char *cp, *var, *val, *line = NULL;
    size_t var_len, val_len, linesize = 0;
    debug_decl(read_env_file, SUDOERS_DEBUG_ENV)

    if ((fp = fopen(path, "r")) == NULL) {
	if (errno != ENOENT)
	    rval = false;
	debug_return_bool(rval);
    }

    while (sudo_parseln(&line, &linesize, NULL, fp) != -1) {
	/* Skip blank or comment lines */
	if (*(var = line) == '\0')
	    continue;

	/* Skip optional "export " */
	if (strncmp(var, "export", 6) == 0 && isspace((unsigned char) var[6])) {
	    var += 7;
	    while (isspace((unsigned char) *var)) {
		var++;
	    }
	}

	/* Must be of the form name=["']value['"] */
	for (val = var; *val != '\0' && *val != '='; val++)
	    ;
	if (var == val || *val != '=')
	    continue;
	var_len = (size_t)(val - var);
	val_len = strlen(++val);

	/* Strip leading and trailing single/double quotes */
	if ((val[0] == '\'' || val[0] == '\"') && val[0] == val[val_len - 1]) {
	    val[val_len - 1] = '\0';
	    val++;
	    val_len -= 2;
	}

	if ((cp = malloc(var_len + 1 + val_len + 1)) == NULL) {
	    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		"unable to allocate memory");
	    /* XXX - no undo on failure */
	    rval = false;
	    break;
	}
	memcpy(cp, var, var_len + 1); /* includes '=' */
	memcpy(cp + var_len + 1, val, val_len + 1); /* includes NUL */

	if (sudo_putenv(cp, true, overwrite) == -1) {
	    /* XXX - no undo on failure */
	    rval = false;
	    break;
	}
    }
    free(line);
    fclose(fp);

    debug_return_bool(rval);
}

bool
init_envtables(void)
{
    struct list_member *cur;
    const char **p;
    debug_decl(init_envtables, SUDOERS_DEBUG_ENV)

    /* Fill in the "env_delete" list. */
    for (p = initial_badenv_table; *p; p++) {
	cur = calloc(1, sizeof(struct list_member));
	if (cur == NULL || (cur->value = strdup(*p)) == NULL) {
	    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		"unable to allocate memory");
	    free(cur);
	    debug_return_bool(false);
	}
	SLIST_INSERT_HEAD(&def_env_delete, cur, entries);
    }

    /* Fill in the "env_check" list. */
    for (p = initial_checkenv_table; *p; p++) {
	cur = calloc(1, sizeof(struct list_member));
	if (cur == NULL || (cur->value = strdup(*p)) == NULL) {
	    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		"unable to allocate memory");
	    free(cur);
	    debug_return_bool(false);
	}
	SLIST_INSERT_HEAD(&def_env_check, cur, entries);
    }

    /* Fill in the "env_keep" list. */
    for (p = initial_keepenv_table; *p; p++) {
	cur = calloc(1, sizeof(struct list_member));
	if (cur == NULL || (cur->value = strdup(*p)) == NULL) {
	    sudo_debug_printf(SUDO_DEBUG_ERROR|SUDO_DEBUG_LINENO,
		"unable to allocate memory");
	    free(cur);
	    debug_return_bool(false);
	}
	SLIST_INSERT_HEAD(&def_env_keep, cur, entries);
    }
    debug_return_bool(true);
}

int
sudoers_hook_getenv(const char *name, char **value, void *closure)
{
    static bool in_progress = false; /* avoid recursion */

    if (in_progress || env.envp == NULL)
	return SUDO_HOOK_RET_NEXT;

    in_progress = true;

    /* Hack to make GNU gettext() find the sudoers locale when needed. */
    if (*name == 'L' && sudoers_getlocale() == SUDOERS_LOCALE_SUDOERS) {
	if (strcmp(name, "LANGUAGE") == 0 || strcmp(name, "LANG") == 0) {
	    *value = NULL;
	    goto done;
	}
	if (strcmp(name, "LC_ALL") == 0 || strcmp(name, "LC_MESSAGES") == 0) {
	    *value = def_sudoers_locale;
	    goto done;
	}
    }

    *value = sudo_getenv_nodebug(name);
done:
    in_progress = false;
    return SUDO_HOOK_RET_STOP;
}

int
sudoers_hook_putenv(char *string, void *closure)
{
    static bool in_progress = false; /* avoid recursion */

    if (in_progress || env.envp == NULL)
	return SUDO_HOOK_RET_NEXT;

    in_progress = true;
    sudo_putenv_nodebug(string, true, true);
    in_progress = false;
    return SUDO_HOOK_RET_STOP;
}

int
sudoers_hook_setenv(const char *name, const char *value, int overwrite, void *closure)
{
    static bool in_progress = false; /* avoid recursion */

    if (in_progress || env.envp == NULL)
	return SUDO_HOOK_RET_NEXT;

    in_progress = true;
    sudo_setenv_nodebug(name, value, overwrite);
    in_progress = false;
    return SUDO_HOOK_RET_STOP;
}

int
sudoers_hook_unsetenv(const char *name, void *closure)
{
    static bool in_progress = false; /* avoid recursion */

    if (in_progress || env.envp == NULL)
	return SUDO_HOOK_RET_NEXT;

    in_progress = true;
    sudo_unsetenv_nodebug(name);
    in_progress = false;
    return SUDO_HOOK_RET_STOP;
}
