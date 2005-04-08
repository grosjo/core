/* Copyright (C) 2002-2003 Timo Sirainen */

#include "lib.h"
#include "ioloop.h"
#include "mail-storage-private.h"

#include <stdlib.h>
#include <time.h>
#include <ctype.h>

/* Message to show to users when critical error occurs */
#define CRITICAL_MSG \
	"Internal error occured. Refer to server log for more information."
#define CRITICAL_MSG_STAMP CRITICAL_MSG " [%Y-%m-%d %H:%M:%S]"

unsigned int mail_storage_module_id = 0;

static array_t ARRAY_DEFINE(storages, struct mail_storage *);

void mail_storage_init(void)
{
	ARRAY_CREATE(&storages, default_pool, struct mail_storage *, 8);
}

void mail_storage_deinit(void)
{
	if (array_is_created(&storages))
		array_free(&storages);
}

void mail_storage_class_register(struct mail_storage *storage_class)
{
	/* append it after the list, so the autodetection order is correct */
	array_append(&storages, &storage_class, 1);
}

void mail_storage_class_unregister(struct mail_storage *storage_class)
{
	struct mail_storage *const *classes;
	unsigned int i, count;

	classes = array_get(&storages, &count);
	for (i = 0; i < count; i++) {
		if (classes[i] == storage_class) {
			array_delete(&storages, i, 1);
			break;
		}
	}
}

static struct mail_storage *mail_storage_find(const char *name)
{
	struct mail_storage *const *classes;
	unsigned int i, count;

	i_assert(name != NULL);

	classes = array_get(&storages, &count);
	for (i = 0; i < count; i++) {
		if (strcasecmp(classes[i]->name, name) == 0)
			return classes[i];
	}
	return NULL;
}

struct mail_storage *
mail_storage_create(const char *name, const char *data, const char *user,
		    enum mail_storage_flags flags,
		    enum mail_storage_lock_method lock_method)
{
	struct mail_storage *storage;

	storage = mail_storage_find(name);
	if (storage != NULL)
		return storage->v.create(data, user, flags, lock_method);
	else
		return NULL;
}

struct mail_storage *
mail_storage_create_default(const char *user, enum mail_storage_flags flags,
			    enum mail_storage_lock_method lock_method)
{
	struct mail_storage *const *classes;
	struct mail_storage *storage;
	unsigned int i, count;

	classes = array_get(&storages, &count);
	for (i = 0; i < count; i++) {
		storage = classes[i]->v.create(NULL, user, flags, lock_method);
		if (storage != NULL)
			return storage;
	}
	return NULL;
}

static struct mail_storage *
mail_storage_autodetect(const char *data, enum mail_storage_flags flags)
{
	struct mail_storage *const *classes;
	unsigned int i, count;

	classes = array_get(&storages, &count);
	for (i = 0; i < count; i++) {
		if (classes[i]->v.autodetect(data, flags))
			return classes[i];
	}
	return NULL;
}

struct mail_storage *
mail_storage_create_with_data(const char *data, const char *user,
			      enum mail_storage_flags flags,
			      enum mail_storage_lock_method lock_method)
{
	struct mail_storage *storage;
	const char *p, *name;

	if (data == NULL || *data == '\0')
		return mail_storage_create_default(user, flags, lock_method);

	/* check if we're in the form of mailformat:data
	   (eg. maildir:Maildir) */
	p = data;
	while (i_isalnum(*p)) p++;

	if (*p == ':') {
		name = t_strdup_until(data, p);
		storage = mail_storage_create(name, p+1, user, flags,
					      lock_method);
	} else {
		storage = mail_storage_autodetect(data, flags);
		if (storage != NULL) {
			storage = storage->v.create(data, user, flags,
						    lock_method);
		}
	}

	return storage;
}

void mail_storage_destroy(struct mail_storage *storage)
{
	i_assert(storage != NULL);

	storage->v.destroy(storage);
}

void mail_storage_clear_error(struct mail_storage *storage)
{
	i_free(storage->error);
	storage->error = NULL;

	storage->syntax_error = FALSE;
}

void mail_storage_set_error(struct mail_storage *storage, const char *fmt, ...)
{
	va_list va;

	i_free(storage->error);

	if (fmt == NULL)
		storage->error = NULL;
	else {
		va_start(va, fmt);
		storage->error = i_strdup_vprintf(fmt, va);
		storage->syntax_error = FALSE;
		va_end(va);
	}
}

void mail_storage_set_syntax_error(struct mail_storage *storage,
				   const char *fmt, ...)
{
	va_list va;

	i_free(storage->error);

	if (fmt == NULL)
		storage->error = NULL;
	else {
		va_start(va, fmt);
		storage->error = i_strdup_vprintf(fmt, va);
		storage->syntax_error = TRUE;
		va_end(va);
	}
}

void mail_storage_set_internal_error(struct mail_storage *storage)
{
	struct tm *tm;
	char str[256];

	tm = localtime(&ioloop_time);

	i_free(storage->error);
	storage->error =
		strftime(str, sizeof(str), CRITICAL_MSG_STAMP, tm) > 0 ?
		i_strdup(str) : i_strdup(CRITICAL_MSG);
	storage->syntax_error = FALSE;
}

void mail_storage_set_critical(struct mail_storage *storage,
			       const char *fmt, ...)
{
	va_list va;

	i_free(storage->error);
	if (fmt == NULL)
		storage->error = NULL;
	else {
		va_start(va, fmt);
		i_error("%s", t_strdup_vprintf(fmt, va));
		va_end(va);

		/* critical errors may contain sensitive data, so let user
		   see only "Internal error" with a timestamp to make it
		   easier to look from log files the actual error message. */
		mail_storage_set_internal_error(storage);
	}
}

char mail_storage_get_hierarchy_sep(struct mail_storage *storage)
{
	return storage->hierarchy_sep;
}

void mail_storage_set_callbacks(struct mail_storage *storage,
				struct mail_storage_callbacks *callbacks,
				void *context)
{
	storage->v.set_callbacks(storage, callbacks, context);
}

int mail_storage_mailbox_create(struct mail_storage *storage, const char *name,
				int directory)
{
	return storage->v.mailbox_create(storage, name, directory);
}

int mail_storage_mailbox_delete(struct mail_storage *storage, const char *name)
{
	return storage->v.mailbox_delete(storage, name);
}

int mail_storage_mailbox_rename(struct mail_storage *storage,
				const char *oldname, const char *newname)
{
	return storage->v.mailbox_rename(storage, oldname, newname);
}

struct mailbox_list_context *
mail_storage_mailbox_list_init(struct mail_storage *storage,
			       const char *ref, const char *mask,
			       enum mailbox_list_flags flags)
{
	return storage->v.mailbox_list_init(storage, ref, mask, flags);
}

struct mailbox_list *
mail_storage_mailbox_list_next(struct mailbox_list_context *ctx)
{
	return ctx->storage->v.mailbox_list_next(ctx);
}

int mail_storage_mailbox_list_deinit(struct mailbox_list_context *ctx)
{
	return ctx->storage->v.mailbox_list_deinit(ctx);
}

int mail_storage_set_subscribed(struct mail_storage *storage,
				const char *name, int set)
{
	return storage->v.set_subscribed(storage, name, set);
}

int mail_storage_get_mailbox_name_status(struct mail_storage *storage,
					 const char *name,
					 enum mailbox_name_status *status)
{
	return storage->v.get_mailbox_name_status(storage, name, status);
}

const char *mail_storage_get_last_error(struct mail_storage *storage,
					int *syntax_error_r)
{
	return storage->v.get_last_error(storage, syntax_error_r);
}

struct mailbox *mailbox_open(struct mail_storage *storage, const char *name,
			     struct istream *input,
			     enum mailbox_open_flags flags)
{
	return storage->v.mailbox_open(storage, name, input, flags);
}

int mailbox_close(struct mailbox *box)
{
	return box->v.close(box);
}

struct mail_storage *mailbox_get_storage(struct mailbox *box)
{
	return box->storage;
}

const char *mailbox_get_name(struct mailbox *box)
{
	return box->name;
}

int mailbox_is_readonly(struct mailbox *box)
{
	return box->v.is_readonly(box);
}

int mailbox_allow_new_keywords(struct mailbox *box)
{
	return box->v.allow_new_keywords(box);
}

int mailbox_get_status(struct mailbox *box,
		       enum mailbox_status_items items,
		       struct mailbox_status *status)
{
	return box->v.get_status(box, items, status);
}

struct mailbox_sync_context *
mailbox_sync_init(struct mailbox *box, enum mailbox_sync_flags flags)
{
	return box->v.sync_init(box, flags);
}

int mailbox_sync_next(struct mailbox_sync_context *ctx,
		      struct mailbox_sync_rec *sync_rec_r)
{
	return ctx->box->v.sync_next(ctx, sync_rec_r);
}

int mailbox_sync_deinit(struct mailbox_sync_context *ctx,
			struct mailbox_status *status_r)
{
	return ctx->box->v.sync_deinit(ctx, status_r);
}

void mailbox_notify_changes(struct mailbox *box, unsigned int min_interval,
			    mailbox_notify_callback_t *callback, void *context)
{
	box->v.notify_changes(box, min_interval, callback, context);
}

struct mail_keywords *
mailbox_keywords_create(struct mailbox_transaction_context *t,
			const char *const keywords[])
{
	return t->box->v.keywords_create(t, keywords);
}

void mailbox_keywords_free(struct mailbox_transaction_context *t,
			   struct mail_keywords *keywords)
{
	t->box->v.keywords_free(t, keywords);
}

int mailbox_get_uids(struct mailbox *box, uint32_t uid1, uint32_t uid2,
		     uint32_t *seq1_r, uint32_t *seq2_r)
{
	return box->v.get_uids(box, uid1, uid2, seq1_r, seq2_r);
}

struct mailbox_header_lookup_ctx *
mailbox_header_lookup_init(struct mailbox *box, const char *const headers[])
{
	return box->v.header_lookup_init(box, headers);
}

void mailbox_header_lookup_deinit(struct mailbox_header_lookup_ctx *ctx)
{
	ctx->box->v.header_lookup_deinit(ctx);
}

int mailbox_search_get_sorting(struct mailbox *box,
			       enum mail_sort_type *sort_program)
{
	return box->v.search_get_sorting(box, sort_program);
}

struct mail_search_context *
mailbox_search_init(struct mailbox_transaction_context *t,
		    const char *charset, struct mail_search_arg *args,
		    const enum mail_sort_type *sort_program)
{
	return t->box->v.search_init(t, charset, args, sort_program);
}

int mailbox_search_deinit(struct mail_search_context *ctx)
{
	return ctx->transaction->box->v.search_deinit(ctx);
}

int mailbox_search_next(struct mail_search_context *ctx, struct mail *mail)
{
	return ctx->transaction->box->v.search_next(ctx, mail);
}

struct mailbox_transaction_context *
mailbox_transaction_begin(struct mailbox *box,
			  enum mailbox_transaction_flags flags)
{
	return box->v.transaction_begin(box, flags);
}

int mailbox_transaction_commit(struct mailbox_transaction_context *t,
			       enum mailbox_sync_flags flags)
{
	return t->box->v.transaction_commit(t, flags);
}

void mailbox_transaction_rollback(struct mailbox_transaction_context *t)
{
	t->box->v.transaction_rollback(t);
}

struct mail_save_context *
mailbox_save_init(struct mailbox_transaction_context *t,
		  enum mail_flags flags, struct mail_keywords *keywords,
		  time_t received_date, int timezone_offset,
		  const char *from_envelope, struct istream *input,
		  int want_mail)
{
	return t->box->v.save_init(t, flags, keywords,
				   received_date, timezone_offset,
				   from_envelope, input, want_mail);
}

int mailbox_save_continue(struct mail_save_context *ctx)
{
	return ctx->transaction->box->v.save_continue(ctx);
}

int mailbox_save_finish(struct mail_save_context *ctx, struct mail *dest_mail)
{
	return ctx->transaction->box->v.save_finish(ctx, dest_mail);
}

void mailbox_save_cancel(struct mail_save_context *ctx)
{
	ctx->transaction->box->v.save_cancel(ctx);
}

int mailbox_copy(struct mailbox_transaction_context *t, struct mail *mail,
		 struct mail *dest_mail)
{
	return t->box->v.copy(t, mail, dest_mail);
}

int mailbox_is_inconsistent(struct mailbox *box)
{
	return box->v.is_inconsistent(box);
}
