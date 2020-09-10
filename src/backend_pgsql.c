/*
 * PAM authentication module for PostgreSQL
 * 
 * Based in part on pam_unix.c of FreeBSD. See COPYRIGHT
 * for licensing details.
 *
 * David D.W. Downey ("pgpkeys") <david-downey@codecastle.com> et al. (see COPYRIGHT)
 * William Grzybowski <william@agencialivre.com.br>
 */

#define _XOPEN_SOURCE 500
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <libpq-fe.h>
#include <sys/types.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <crypt.h>
#include <gcrypt.h>

#include <openssl/evp.h>

/* Base 64 encoding / decoding */
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include "backend_pgsql.h"
#include "pam_pgsql.h"

static char* calc_PBKDF2_HMAC_SHA256(const char* passwd,
									 const unsigned char* salt,
									 int32_t n_iter,
									 uint32_t key_len);

static char *base64_encode(const unsigned char *input, int length);
static unsigned char *base64_decode(const char *input, int length);

static char *
crypt_makesalt(pw_scheme scheme);

/* very private: used only in get_module_options */
static char *
build_conninfo(modopt_t *options)
{
    char *str;

	 if(options == NULL)
		 return NULL;

	 str = (char *) malloc(sizeof(char)*512);
    memset(str, 0, 512);

    /* SAFE */
	 if(options->db) {
		strcat(str, "dbname=");
		strncat(str, options->db, strlen(options->db));
	 }

	if(options->host) {
		strcat(str, " host=");
		strncat(str, options->host, strlen(options->host));
	}
	if(options->port) {
		strcat(str, " port=");
		strncat(str, options->port, strlen(options->port));
	}    
	if(options->timeout) {
		strcat(str, " connect_timeout=");
		strncat(str, options->timeout, strlen(options->timeout));
	}
	if(options->user) {
		strcat(str, " user=");
		strncat(str, options->user, strlen(options->user));
	}
	if(options->passwd) {
		strcat(str, " password=");
		strncat(str, options->passwd, strlen(options->passwd));
	}
	if(options->sslmode) {
		strcat(str, " sslmode=");
		strncat(str, options->sslmode, strlen(options->sslmode));
	}

	return str;
}

/* private: open connection to PostgreSQL */
PGconn *
db_connect(modopt_t *options)
{
	PGconn *conn;
	if(options->connstr == NULL)
		options->connstr = build_conninfo(options);

	conn = PQconnectdb(options->connstr);
	if(PQstatus(conn) != CONNECTION_OK) {
		SYSLOG("PostgreSQL connection failed: '%s'", PQerrorMessage(conn));
		return NULL;
	}
	return conn;
}

/* private: expand query; partially stolen from mailutils */

static int
expand_query (char **command, const char** values, const char *query, const char *service, const char *user, const char *passwd, const char *rhost, const char *raddr)
{
	char *p, *q, *res;
	unsigned int len;
	unsigned int nparm=0;
  
	if (!query) {
		*command = NULL;
		return 0;
	}
	/* Compute resulting query length */
	for (len = 0, p = (char *) query; *p; ) {
		if (*p == '%') {
			if (p[1] == 'u' || p[1] == 'p' || p[1] == 's') {
				len += 4; /*we allow 128 tokens max*/
				p += 2;
				continue;
			} else if (p[1] == '%') {
				len++;
				p += 2;
				continue;
			}
		}
		len++;
		p++;  
	}
	res = malloc (len + 1);
	if (!res) {
		*command = NULL;
		return 0;
	}
	for (p = (char *) query, q = res; *p; ) {
		if (*p == '%') {
			switch (*++p) {
				case 'u': {
					sprintf(q, "$%i", ++nparm);
					values[nparm-1] = user;
					q += strlen (q);
					p++;
				}
				break;
				case 'p': {
					sprintf(q, "$%i", ++nparm);
					values[nparm-1] = passwd;
					q += strlen (q);
					p++;
				}
				break;
				case 's': {
					sprintf(q, "$%i", ++nparm);
					values[nparm-1] = service;
					q += strlen (q);
					p++;
				}
				break;
				case 'h': {
					sprintf(q, "$%i", ++nparm);
					values[nparm-1] = rhost;
					q += strlen (q);
					p++;
				}
				break;
				case 'i': {
					sprintf(q, "$%i", ++nparm);
					values[nparm-1] = raddr;
					q += strlen (q);
					p++;
					if (!raddr) {
						if (strchr(rhost, '.') != NULL) {
							*command = NULL;
							free (res);
							return 0;
						}
					}
				}
				break;
				case '%':
				default:
					*q++ = *p++;
				break;
			}
		} else	*q++ = *p++;
	 }
	 *q = 0;
	 
	 *command = res;
	 values[nparm] = NULL; 
	 return nparm;
}

/* private: execute query */
int
pg_execParam(PGconn *conn, PGresult **res, 
        const char *query, const char *service, const char *user, const char *passwd, const char *rhost)
{
	int nparm = 0;
	const char *values[128];
	char *command, *raddr;
	struct hostent *hentry;

	if (!conn) 
		return PAM_AUTHINFO_UNAVAIL;
	bzero(values, sizeof(*values));
	
	raddr = NULL;
	
	if(rhost != NULL && (hentry = gethostbyname(rhost)) != NULL) {
		/* Make IP string */
		raddr = malloc(INET_ADDRSTRLEN);
		inet_ntop(AF_INET, hentry->h_addr_list[0], raddr, INET_ADDRSTRLEN);
	}
	
	nparm = expand_query(&command, values, query, service, user, passwd, rhost, raddr);
	if (command == NULL) 
		return PAM_AUTH_ERR;
	
	*res = PQexecParams(conn, command, nparm, 0, values, 0, 0, 0);
	free (command);
	free (raddr);
    
	if(PQresultStatus(*res) != PGRES_COMMAND_OK && PQresultStatus(*res) != PGRES_TUPLES_OK) {
		SYSLOG("PostgreSQL query failed: '%s'", PQresultErrorMessage(*res));
		return PAM_AUTHINFO_UNAVAIL;
	}
	return PAM_SUCCESS;
}

/* private: convert an integer to a radix 64 character */
static int
i64c(int i)
{
	if (i <= 0)
		return ('.');
	if (i == 1)
		return ('/');
	if (i >= 2 && i < 12)
		return ('0' - 2 + i);
	if (i >= 12 && i < 38)
		return ('A' - 12 + i);
	if (i >= 38 && i < 63)
		return ('a' - 38 + i);
	return ('z');
}

/* authenticate user and passwd against database */
int
backend_authenticate(const char *service, const char *user, const char *passwd, const char *rhost, modopt_t *options)
{
	PGresult *res;
	PGconn *conn;
	int rc, row_count;
	char *tmp;

	if (!(conn = db_connect(options)))
		return PAM_AUTH_ERR;

	DBGLOG("query: %s", options->query_auth);
	rc = PAM_AUTH_ERR;	
	if (pg_execParam(conn, &res, options->query_auth, service, user, passwd, rhost) == PAM_SUCCESS) {
		row_count = PQntuples(res);
		if (row_count == 0) {
			rc = PAM_USER_UNKNOWN;
		} else {
			for (int i = 0; i < row_count && rc != PAM_SUCCESS; i++) {
				if (!PQgetisnull(res, i, 0)) {
					char *stored_pw = PQgetvalue(res, i, 0);
					char *stored_salt = (char *)NULL;

					// Get stored salt as second column if it exists
					if (!PQgetisnull(res, i, 1)) {
						stored_salt = PQgetvalue(res, i, 1);
					}

					if (options->pw_type == PW_FUNCTION) {
						if (!strcmp(stored_pw, "t")) {
							rc = PAM_SUCCESS;
						}
					} else {
						tmp = password_encrypt(options, user, passwd, stored_pw, stored_salt);
						SYSLOG("backend_authenticate: tmp=%s, stored_pw=%s;", tmp, stored_pw);
						if (tmp != NULL && !strcmp(stored_pw, tmp)) {
							rc = PAM_SUCCESS;
						}
						free (tmp);
					}
				}
			}
		}
		PQclear(res);
	}
	PQfinish(conn);
	return rc;
}

/* private: encrypt password using the preferred encryption scheme */
char *
password_encrypt(modopt_t *options, const char *user, const char *pass, const char *salt,
				 const char *stored_salt)
{
	char *s = NULL;

	switch(options->pw_type) {
		case PW_CRYPT:
		case PW_CRYPT_MD5:
		case PW_CRYPT_SHA512: {
			char *c = NULL;
			if (salt==NULL) {
				c = crypt(pass, crypt_makesalt(options->pw_type));
			} else {
				c = crypt(pass, salt);
			}
			if (c!=NULL) {
				s = strdup(c);
			}
		}
		break;
		case PW_MD5: {
			unsigned char hash[16] = { 0, }; /* 16 is the md5 block size */
			int i;
			s = (char *) malloc(33); /* 32 bytes + 1 byte for \0 */

			gcry_md_hash_buffer(GCRY_MD_MD5, hash, pass, strlen(pass));

			for(i = 0; i < sizeof(hash); i++)
				sprintf(&s[i * 2], "%.2x", hash[i]);
		}
		break;
		case PW_MD5_POSTGRES: {
			/* This is the md5 variant used by postgres shadow table.
			cleartext is password||user
			returned value is md5||md5hash(password||user)
			*/
			unsigned char hash[16] = { 0, }; /* 16 is the md5 block size */
			int i;
			s = (char *) malloc(36); /* 3 bytes for "md5" + 32 bytes for the hash + 1 byte for \0 */
			memcpy(s, "md5", 3);

			size_t unencoded_length;
			char *unencoded;

			unencoded_length = strlen(pass)+strlen(user);
			unencoded = malloc(unencoded_length+1);
			sprintf(unencoded, "%s%s", pass, user);

			gcry_md_hash_buffer(GCRY_MD_MD5, hash, unencoded, strlen(unencoded));
			for(i = 0; i < sizeof(hash); i++)
				sprintf(&s[(i * 2) + 3], "%.2x", hash[i]);

			free(unencoded);

		}
		break;
		case PW_SHA1: {
			unsigned char hash[20] = { 0, }; /* 20 is the sha1 block size */
			int i;
			s = (char *) malloc(41); /* 40 bytes + 1 byte for \0 */

			gcry_md_hash_buffer(GCRY_MD_SHA1, hash, pass, strlen(pass));

			for(i = 0; i < sizeof(hash); i++)
				sprintf(&s[i * 2], "%.2x", hash[i]);
		}
		break;
		case PW_CLEAR:
		case PW_FUNCTION:
		case PW_PBKDF2: {
			char *encoded_passwd = (char *)NULL; 
		    unsigned char *decoded_salt = base64_decode(stored_salt, strlen(stored_salt));

			encoded_passwd = calc_PBKDF2_HMAC_SHA256(pass, decoded_salt, 27500, 64);
			if (! encoded_passwd) {
				return (char *)NULL;
			}
			free(decoded_salt);
			s = strdup((char *)encoded_passwd);
		}
		break;
		default:
			s = strdup(pass);
	}
	return s;
}

static char *
crypt_makesalt(pw_scheme scheme)
{
	static char result[12];
	int len,pos;
	struct timeval now;

	if(scheme==PW_CRYPT){
		len=2;
		pos=0;
	} else if(scheme==PW_CRYPT_SHA512) { /* PW_CRYPT_SHA512 */
		strcpy (result, "$6$");
		len = 11;
		pos = 3;
	} else { /* PW_CRYPT_MD5 */
		strcpy(result,"$1$");
		len=11;
		pos=3;
	}
	gettimeofday(&now,NULL);
	srandom(now.tv_sec*10000+now.tv_usec/100+clock());
	while(pos<len)result[pos++]=i64c(random()&63);
	result[len]=0;
	return result;
}

/*
* Calculate PBKDF2 SHA 256 hash of input password based on salt and number of
* iterations set.  Return base 64 encoded result.  Caller responsible for 
* freeing memory of result.
*/
static char* calc_PBKDF2_HMAC_SHA256(const char* passwd,
									 const unsigned char* salt,
									 int32_t n_iter,
									 uint32_t key_len)
{
    char func_name[] = "calc_PBKDF2_HMAC_SHA256";
	char *b64_result = (char *)NULL;
	unsigned char *digest = (unsigned char *)calloc(key_len, 
													sizeof(unsigned char));
	if (! digest) {
		SYSLOG("%s: error allocating memory for digest buffer.", func_name);
		return (char *)NULL;
	}

    if (! PKCS5_PBKDF2_HMAC(passwd, strlen(passwd), 
	                        salt, strlen((char *)salt),
                      		n_iter, EVP_sha256(), key_len, digest)) {
		SYSLOG("%s: error applying PBKDF2 hashing for input password", 
			   func_name);
		return (char *)NULL;
	}

	b64_result = base64_encode(digest, strlen((char *)digest));
	free(digest);

    return b64_result;
}


// Thanks to:
// http://www.ioncannon.net/programming/34/howto-base64-encode-with-cc-and-openssl/
static char *base64_encode(const unsigned char *input, int length) {
	char func_name[] = "base64_encode";
    BIO *bmem = (BIO *)NULL;
	BIO *b64 = (BIO *)NULL;
    BUF_MEM *bptr = (BUF_MEM *)NULL;
    int i = 0;
    char *p_buff_out = (char *)NULL;
	char *buff_out = (char *)NULL;

    b64 = BIO_new(BIO_f_base64());
    bmem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, bmem);
    BIO_write(b64, input, length);
    BIO_flush(b64);
    BIO_get_mem_ptr(b64, &bptr);

	buff_out = (char *)calloc(bptr->length, sizeof(char));
    if (! buff_out) {
        SYSLOG("%s: error allocating memory for output buffer", func_name);
        return (char *)NULL;
    }

	for (p_buff_out=buff_out, i=0; i < bptr->length; i++) {
        if (bptr->data[i] != '\n') {
            *p_buff_out = bptr->data[i];
            p_buff_out++;
        }
    }
    buff_out[bptr->length-1] = 0;
 
    BIO_free_all(b64);
 
    return buff_out;
}


// Thanks to:
// http://www.ioncannon.net/programming/122/howto-base64-decode-with-cc-and-openssl/
static unsigned char *base64_decode(const char *input, int length)
{
    const char func_name[] = "base64_decode";
	BIO *b64, *bmem;
    char *buff_out = (char *)malloc(length*sizeof(char));
    if (! buff_out) {
        SYSLOG("%s: error allocating memory for output buffer", func_name);
        return (unsigned char *)NULL;
    }
 
	memset(buff_out, 0, length);

    b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bmem = BIO_new_mem_buf(input, length);
    bmem = BIO_push(b64, bmem);

    BIO_read(bmem, buff_out, length);

    BIO_free_all(bmem);
 
    return (unsigned char *)buff_out;
}