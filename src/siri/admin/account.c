/*
 * account.c - SiriDB Administrative User.
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2017, Transceptor Technology
 *
 * changes
 *  - initial version, 16-03-2017
 *
 */
#include <siri/admin/account.h>
#include <stddef.h>
#include <owcrypt/owcrypt.h>
#include <xpath/xpath.h>
#include <siri/siri.h>
#include <stdarg.h>
#include <logger/logger.h>

#define SIRI_ADMIN_USER_SCHEMA 1
#define FILENAME ".accounts.dat"

#define DEFAULT_USER "iris"
#define DEFAULT_PASSWORD "siri"

static int USER_free(siri_admin_account_t * account, void * args);
static int USER_save(siri_admin_account_t * account, qp_fpacker_t * fpacker);
static void USER_msg(char * err_msg, char * fmt, ...);
inline static int USER_cmp(
        siri_admin_account_t * account,
        qp_obj_t * qp_account);

/*
 * Initialize siri->accounts. Returns 0 if successful or -1 in case of an error
 * (a signal might be raised in case of an error)
 */
int siri_admin_account_init(siri_t * siri)
{
    qp_unpacker_t * unpacker;
    qp_obj_t qp_schema;
    qp_obj_t qp_account;
    qp_obj_t qp_password;
    int rc = 0;

    LOGC("Init...");

    /* get administrative accounts file name */
    char fn[strlen(siri->cfg->default_db_path) + strlen(FILENAME) + 1];

    /* initialize linked list */
    siri->accounts = llist_new();
    if (siri->accounts == NULL)
    {
        return -1;
    }

    /* make filename */
    sprintf(fn, "%s%s", siri->cfg->default_db_path, FILENAME);

    if (!xpath_file_exist(fn))
    {
        /* missing file, lets create the first account */
        qp_account.via.raw = DEFAULT_USER;
        qp_account.len = 4;
        qp_password.via.raw = DEFAULT_PASSWORD;
        qp_password.len = 4;

        return (siri_admin_account_new(
                siri,
                &qp_account,
                &qp_password,
                0,
                NULL) || siri_admin_account_save(siri, NULL));
    }

    if ((unpacker = qp_unpacker_ff(fn)) == NULL)
    {
        return -1;  /* a signal is raised is case of a memory error */
    }

    if (    !qp_is_array(qp_next(unpacker, NULL)) ||
            qp_next(unpacker, &qp_schema) != QP_INT64 ||
            qp_schema.via.int64 != SIRI_ADMIN_USER_SCHEMA)
    {
        log_critical("Invalid schema detected in '%s'", fn);
        qp_unpacker_ff_free(unpacker);
        return -1;
    }

    while ( rc == 0 &&
            qp_is_array(qp_next(unpacker, NULL)) &&
            qp_next(unpacker, &qp_account) == QP_RAW &&
            qp_next(unpacker, &qp_password) == QP_RAW &&
            qp_password.len > 12)  /* old and new passwords are > 12 */
    {
        rc = siri_admin_account_new(siri, &qp_account, &qp_password, 1, NULL);
    }

    qp_unpacker_ff_free(unpacker);
    return rc;
}

/*
 * Creates a new administrative account and returns 0 if successful. In case of
 * an error, -1 is returned, err_msg is set and a signal might be raised.
 *
 * When successful, the account is added to the siri->accounts linked list.
 *
 * is_encrypted should be zero if the password is not encrypted yet.
 *
 * Note: the account will not be saved to disk. Call siri_admin_account_save()
 *       to save a new administrative account.
 */
int siri_admin_account_new(
        siri_t * siri,
        qp_obj_t * qp_account,
        qp_obj_t * qp_password,
        int is_encrypted,
        char * err_msg)
{
    siri_admin_account_t * account;

    account = (siri_admin_account_t *) llist_get(
            siri->accounts,
            (llist_cb) USER_cmp,
            (void *) qp_account);

    if (account != NULL)
    {
        USER_msg(
                err_msg,
                "server account '%.*s' already exists",
                (int) qp_account->len,
                qp_account->via.raw);
        return -1;
    }

    account = (siri_admin_account_t *) malloc(sizeof(siri_admin_account_t));
    if (account == NULL)
    {
        USER_msg(err_msg, "memory allocation error");
        return -1;
    }

    account->account = strndup(qp_account->via.raw, qp_account->len);
    account->password = strndup(qp_password->via.raw, qp_password->len);

    if (!is_encrypted && account->password != NULL)
    {
        char encrypted[OWCRYPT_SZ];
        char salt[OWCRYPT_SALT_SZ];

        /* generate a random salt */
        owcrypt_gen_salt(salt);

        /* encrypt the accounts password */
        owcrypt(account->password, salt, encrypted);

        /* replace with encrypted password */
        free(account->password);
        account->password = strdup(encrypted);
    }

    if (    account->account == NULL ||
            account->password == NULL ||
            llist_append(siri->accounts, account))
    {
        USER_msg(err_msg, "memory allocation error");
        return -1;
    }
    return 0;
}

/*
 * Returns 0 if the account/password is valid or another value if not.
 */
int siri_admin_account_check(
        siri_t * siri,
        qp_obj_t * qp_account,
        qp_obj_t * qp_password,
        char * err_msg)
{
    siri_admin_account_t * account;
    char pw[OWCRYPT_SZ];
    char * password;

    account = (siri_admin_account_t *) llist_get(
            siri->accounts,
            (llist_cb) USER_cmp,
            (void *) qp_account);

    if (account == NULL)
    {
        USER_msg(
                err_msg,
                "cannot find server account '%.*s'",
                (int) qp_account->len,
                qp_account->via.raw);
        return -1;
    }

    password= strndup(qp_password->via.raw, qp_password->len);

    if (password == NULL)
    {
        USER_msg(err_msg, "memory allocation error");
        return -1;
    }

    owcrypt(password, account->password, pw);
    free(password);

    if (strcmp(pw, account->password))
    {
        USER_msg(
                err_msg,
                "incorrect password for server account '%.*s",
                (int) qp_account->len,
                qp_account->via.raw);
        return -1;
    }

    return 0;
}

/*
 * Returns 0 if the password is successful changed or -1 if not.
 *
 * Note: the password change is not saved, call siri_admin_account_save().
 */
int siri_admin_account_change_password(
        siri_t * siri,
        qp_obj_t * qp_account,
        qp_obj_t * qp_password,
        char * err_msg)
{
    siri_admin_account_t * account;
    char encrypted[OWCRYPT_SZ];
    char salt[OWCRYPT_SALT_SZ];
    char * password;

    account = (siri_admin_account_t *) llist_get(
            siri->accounts,
            (llist_cb) USER_cmp,
            (void *) qp_account);

    if (account == NULL)
    {
        USER_msg(
                err_msg,
                "cannot find server account '%.*s'",
                (int) qp_account->len,
                qp_account->via.raw);
        return -1;
    }

    password= strndup(qp_password->via.raw, qp_password->len);

    if (password == NULL)
    {
        USER_msg(err_msg, "memory allocation error");
        return -1;
    }

    /* generate a random salt */
    owcrypt_gen_salt(salt);

    /* encrypt the accounts password */
    owcrypt(password, salt, encrypted);

    free(password);

    password = strdup(encrypted);
    if (password == NULL)
    {
        USER_msg(err_msg, "memory allocation error");
        return -1;
    }

    /* replace account password with new encrypted password */
    free(account->password);
    account->password = password;

    return 0;
}

/*
 * Returns 0 if dropped or -1 in case the account was not found.
 *
 * Note: accounts are not saved, call siri_admin_account_save().
 */
int siri_admin_account_drop(
        siri_t * siri,
        qp_obj_t * qp_account,
        char * err_msg)
{
    siri_admin_account_t * account;
    account = (siri_admin_account_t *) llist_remove(
            siri->accounts,
            (llist_cb) USER_cmp,
            (void *) qp_account);
    if (account == NULL)
    {
        USER_msg(
                err_msg,
                "cannot find server account '%.*s'",
                (int) qp_account->len,
                qp_account->via.raw);
        return -1;
    }

    USER_free(account, NULL);
    return 0;
}

/*
 * Destroy administrative accounts. siri->accounts is allowed to be NULL.
 */
void siri_admin_account_destroy(siri_t * siri)
{
    if (siri->accounts != NULL)
    {
        llist_free_cb(siri->accounts, (llist_cb) USER_free, NULL);
    }
}

/*
 * Returns 0 if successful or EOF if not.
 */
int siri_admin_account_save(siri_t * siri, char * err_msg)
{
    qp_fpacker_t * fpacker;

    /* get administrative accounts file name */
    char fn[strlen(siri->cfg->default_db_path) + strlen(FILENAME) + 1];

    /* make filename */
    sprintf(fn, "%s%s", siri->cfg->default_db_path, FILENAME);

    if (
        /* open a new account file */
        (fpacker = qp_open(fn, "w")) == NULL ||

        /* open a new array */
        qp_fadd_type(fpacker, QP_ARRAY_OPEN) ||

        /* write the current schema */
        qp_fadd_int16(fpacker, SIRI_ADMIN_USER_SCHEMA) ||

        /* we can and should skip this if we have no accounts to save */
        llist_walk(siri->accounts, (llist_cb) USER_save, fpacker) ||

        /* close file pointer */
        qp_close(fpacker))
    {
        USER_msg(err_msg, "error saving server accounts");
        return EOF;
    }
    return 0;
}

static int USER_free(siri_admin_account_t * account, void * args)
{
    free(account->account);
    free(account->password);
    free(account);
    return 0;
}

/*
 * Returns 0 if successful and -1 in case an error occurred.
 */
static int USER_save(siri_admin_account_t * account, qp_fpacker_t * fpacker)
{
    int rc = 0;

    rc += qp_fadd_type(fpacker, QP_ARRAY2);
    rc += qp_fadd_string(fpacker, account->account);
    rc += qp_fadd_string(fpacker, account->password);

    return rc;
}

static void USER_msg(char * err_msg, char * fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    if (err_msg != NULL)
    {
        vsnprintf(err_msg, SIRI_MAX_SIZE_ERR_MSG, fmt, args);
    }
    else
    {
        log_error(fmt, args);
    }
    va_end(args);
}

inline static int USER_cmp(
        siri_admin_account_t * account,
        qp_obj_t * qp_account)
{
    size_t len = strlen(account->account);
    return (len == qp_account->len && strncmp(
            account->account,
            qp_account->via.raw,
            len) == 0);
}