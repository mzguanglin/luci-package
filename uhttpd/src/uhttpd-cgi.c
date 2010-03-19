#include "uhttpd.h"
#include "uhttpd-cgi.h"
#include "uhttpd-utils.h"

static struct http_response * uh_cgi_header_parse(char *buf, int len, int *off)
{
	char *bufptr = NULL;
	char *hdrname = NULL;
	int hdrcount = 0;
	int pos = 0;

	static struct http_response res;


	if( ((bufptr = strfind(buf, len, "\r\n\r\n", 4)) != NULL) ||
	    ((bufptr = strfind(buf, len, "\n\n", 2)) != NULL)
	) {
		*off = (int)(bufptr - buf) + ((bufptr[1] == '\r') ? 4 : 2);

		memset(&res, 0, sizeof(res));

		res.statuscode = 200;
		res.statusmsg  = "OK";

		bufptr = &buf[0];

		for( pos = 0; pos < len; pos++ )
		{
			if( !hdrname && (buf[pos] == ':') )
			{
				buf[pos++] = 0;

				if( (pos < len) && (buf[pos] == ' ') )
					pos++;

				if( pos < len )
				{
					hdrname = bufptr;
					bufptr = &buf[pos];
				}
			}

			else if( (buf[pos] == '\r') || (buf[pos] == '\n') )
			{
				buf[pos++] = 0;

				if( ! hdrname )
					break;

				if( (pos < len) && (buf[pos] == '\n') )
					pos++;

				if( pos < len )
				{
					if( (hdrcount + 1) < array_size(res.headers) )
					{
						if( ! strcasecmp(hdrname, "Status") )
						{
							res.statuscode = atoi(bufptr);

							if( res.statuscode < 100 )
								res.statuscode = 200;

							if( ((bufptr = strchr(bufptr, ' ')) != NULL) && (&bufptr[1] != 0) )
								res.statusmsg = &bufptr[1];
						}
						else
						{
							res.headers[hdrcount++] = hdrname;
							res.headers[hdrcount++] = bufptr;
						}

						bufptr = &buf[pos];
						hdrname = NULL;
					}
					else
					{
						return NULL;
					}
				}
			}
		}

		return &res;
	}

	return NULL;
}

static char * uh_cgi_header_lookup(struct http_response *res, const char *hdrname)
{
	int i;

	foreach_header(i, res->headers)
	{
		if( ! strcasecmp(res->headers[i], hdrname) )
			return res->headers[i+1];
	}

	return NULL;
}

static void uh_cgi_error_500(struct client *cl, struct http_request *req, const char *message)
{
	uh_http_sendf(cl, NULL,
		"HTTP/%.1f 500 Internal Server Error\r\n"
		"Content-Type: text/plain\r\n%s\r\n",
			req->version, 
			(req->version > 1.0)
				? "Transfer-Encoding: chunked\r\n" : ""
	);

	uh_http_send(cl, req, message, -1);
}


void uh_cgi_request(struct client *cl, struct http_request *req)
{
	int i, hdroff, bufoff;
	int hdrlen = 0;
	int buflen = 0;
	int fd_max = 0;
	int content_length = 0;
	int header_sent = 0;

	int rfd[2] = { 0, 0 };
	int wfd[2] = { 0, 0 };

	char buf[UH_LIMIT_MSGHEAD];
	char hdr[UH_LIMIT_MSGHEAD];

	fd_set reader;
	fd_set writer;

	struct timeval timeout;
	struct http_response *res;
	struct uh_path_info *pi;


	/* spawn pipes for me->child, child->me */
	if( (pipe(rfd) < 0) || (pipe(wfd) < 0) )
	{
		uh_http_sendhf(cl, 500, "Internal Server Error",
			"Failed to create pipe: %s", strerror(errno));

		if( rfd[0] > 0 ) close(rfd[0]);
		if( rfd[1] > 0 ) close(rfd[1]);
		if( wfd[0] > 0 ) close(wfd[0]);
		if( wfd[1] > 0 ) close(wfd[1]);

		return;
	}

	/* fork off child process */
	switch( fork() )
	{
		/* oops */
		case -1:
			uh_http_sendhf(cl, 500, "Internal Server Error",
				"Failed to fork child: %s", strerror(errno));
			return;

		/* exec child */
		case 0:
			/* child */
			close(rfd[0]);
			close(wfd[1]);

			/* patch stdout and stdin to pipes */
			dup2(rfd[1], 1);
			dup2(wfd[0], 0);

			if( (pi = uh_path_lookup(cl, req->url)) != NULL )
			{
				/* check for regular, world-executable file */
				if( (pi->stat.st_mode & S_IFREG) &&
				    (pi->stat.st_mode & S_IXOTH)
				) {
					/* build environment */
					clearenv();

					/* common information */
					setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
					setenv("SERVER_SOFTWARE", "uHTTPd", 1);
					setenv("PATH", "/sbin:/usr/sbin:/bin:/usr/bin", 1);

#ifdef HAVE_TLS
					/* https? */
					if( cl->tls )
						setenv("HTTPS", "on", 1);
#endif

					/* addresses */
					setenv("SERVER_NAME", sa_straddr(&cl->servaddr), 1);
					setenv("SERVER_ADDR", sa_straddr(&cl->servaddr), 1);
					setenv("SERVER_PORT", sa_strport(&cl->servaddr), 1);
					setenv("REMOTE_HOST", sa_straddr(&cl->peeraddr), 1);
					setenv("REMOTE_ADDR", sa_straddr(&cl->peeraddr), 1);
					setenv("REMOTE_PORT", sa_strport(&cl->peeraddr), 1);

					/* path information */
					setenv("SCRIPT_NAME", pi->name, 1);
					setenv("SCRIPT_FILENAME", pi->phys, 1);
					setenv("SCRIPT_WORKDIR", pi->wdir, 1);	/* nonstandard */
					setenv("DOCUMENT_ROOT", pi->root, 1);
					setenv("QUERY_STRING", pi->query ? pi->query : "", 1);

					if( pi->info )
						setenv("PATH_INFO", pi->info, 1);


					/* http version */
					if( req->version > 1.0 )
						setenv("SERVER_PROTOCOL", "HTTP/1.1", 1);
					else
						setenv("SERVER_PROTOCOL", "HTTP/1.0", 1);

					/* request method */
					switch( req->method )
					{
						case UH_HTTP_MSG_GET:
							setenv("REQUEST_METHOD", "GET", 1);
							break;

						case UH_HTTP_MSG_HEAD:
							setenv("REQUEST_METHOD", "HEAD", 1);
							break;

						case UH_HTTP_MSG_POST:
							setenv("REQUEST_METHOD", "POST", 1);
							break;
					}

					/* request url */
					setenv("REQUEST_URI", req->url, 1);

					/* request message headers */
					foreach_header(i, req->headers)
					{
						if( ! strcasecmp(req->headers[i], "Accept") )
							setenv("HTTP_ACCEPT", req->headers[i+1], 1);

						else if( ! strcasecmp(req->headers[i], "Accept-Charset") )
							setenv("HTTP_ACCEPT_CHARSET", req->headers[i+1], 1);

						else if( ! strcasecmp(req->headers[i], "Accept-Encoding") )
							setenv("HTTP_ACCEPT_ENCODING", req->headers[i+1], 1);

						else if( ! strcasecmp(req->headers[i], "Accept-Language") )
							setenv("HTTP_ACCEPT_LANGUAGE", req->headers[i+1], 1);

						else if( ! strcasecmp(req->headers[i], "Authorization") )
							setenv("HTTP_AUTHORIZATION", req->headers[i+1], 1);

						else if( ! strcasecmp(req->headers[i], "Connection") )
							setenv("HTTP_CONNECTION", req->headers[i+1], 1);

						else if( ! strcasecmp(req->headers[i], "Cookie") )
							setenv("HTTP_COOKIE", req->headers[i+1], 1);

						else if( ! strcasecmp(req->headers[i], "Host") )
							setenv("HTTP_HOST", req->headers[i+1], 1);

						else if( ! strcasecmp(req->headers[i], "Referer") )
							setenv("HTTP_REFERER", req->headers[i+1], 1);

						else if( ! strcasecmp(req->headers[i], "User-Agent") )
							setenv("HTTP_USER_AGENT", req->headers[i+1], 1);

						else if( ! strcasecmp(req->headers[i], "Content-Type") )
							setenv("CONTENT_TYPE", req->headers[i+1], 1);

						else if( ! strcasecmp(req->headers[i], "Content-Length") )
							setenv("CONTENT_LENGTH", req->headers[i+1], 1);
					}


					/* execute child code ... */
					if( chdir(pi->wdir) )
						perror("chdir()");

					execl(pi->phys, pi->phys, NULL);

					/* in case it fails ... */
					printf(
						"Status: 500 Internal Server Error\r\n\r\n"
						"Unable to launch the requested CGI program:\n"
						"  %s: %s\n",
							pi->phys, strerror(errno)
					);
				}

				/* 403 */
				else
				{
					printf(
						"Status: 403 Forbidden\r\n\r\n"
						"Access to this resource is forbidden\n"
					);
				}
			}

			/* 404 */
			else
			{
				printf(
					"Status: 404 Not Found\r\n\r\n"
					"Unable to launch the requested CGI program:\n"
					"  No such file or directory\n"
				);
			}

			close(wfd[0]);
			close(rfd[1]);
			exit(0);

			break;

		/* parent; handle I/O relaying */
		default:
			/* close unneeded pipe ends */
			close(rfd[1]);
			close(wfd[0]);

			/* max watch fd */
			fd_max = max(rfd[0], wfd[1]) + 1;

			/* find content length */
			if( req->method == UH_HTTP_MSG_POST )
			{
				foreach_header(i, req->headers)
				{
					if( ! strcasecmp(req->headers[i], "Content-Length") )
					{
						content_length = atoi(req->headers[i+1]);
						break;
					}
				}
			}


			memset(hdr, 0, sizeof(hdr));

			/* I/O loop, watch our pipe ends and dispatch child reads/writes from/to socket */
			while( 1 )
			{
				FD_ZERO(&reader);
				FD_ZERO(&writer);

				FD_SET(rfd[0], &reader);
				FD_SET(wfd[1], &writer);

				timeout.tv_sec = 3;
				timeout.tv_usec = 0;

				/* wait until we can read or write or both */
				if( select(fd_max, &reader, (content_length > -1) ? &writer : NULL, NULL, &timeout) > 0 )
				{
					/* ready to write to cgi program */
					if( FD_ISSET(wfd[1], &writer) )
					{
						/* there is unread post data waiting */
						if( content_length > 0 )
						{
							/* read it from socket ... */
							if( (buflen = uh_tcp_recv(cl, buf, min(content_length, sizeof(buf)))) > 0 )
							{
								/* ... and write it to child's stdin */
								if( write(wfd[1], buf, buflen) < 0 )
									perror("write()");

								content_length -= buflen;
							}

							/* unexpected eof! */
							else
							{
								if( write(wfd[1], "", 0) < 0 )
									perror("write()");

								content_length = 0;
							}
						}

						/* there is no more post data, close pipe to child's stdin */
						else
						{
							close(wfd[1]);
							content_length = -1;
						}
					}

					/* ready to read from cgi program */
					if( FD_ISSET(rfd[0], &reader) )
					{
						/* read data from child ... */
						if( (buflen = read(rfd[0], buf, sizeof(buf))) > 0 )
						{
							/* we have not pushed out headers yet, parse input */
							if( ! header_sent )
							{
								/* head buffer not full and no end yet */
								if( hdrlen < sizeof(hdr) )
								{
									bufoff = min(buflen, sizeof(hdr) - hdrlen);
									memcpy(&hdr[hdrlen], buf, bufoff);
									hdrlen += bufoff;
								}
								else
								{
									bufoff = 0;
								}


								/* try to parse header ... */
								if( (res = uh_cgi_header_parse(hdr, hdrlen, &hdroff)) != NULL )
								{
									/* write status */
									uh_http_sendf(cl, NULL, "HTTP/%.1f %03d %s\r\n",
										req->version, res->statuscode, res->statusmsg);

									/* add Content-Type if no Location or Content-Type */
									if( !uh_cgi_header_lookup(res, "Location") &&
									    !uh_cgi_header_lookup(res, "Content-Type")
									) {
										uh_http_send(cl, NULL,
											"Content-Type: text/plain\r\n", -1);
									}

									/* if request was HTTP 1.1 we'll respond chunked */
									if( (req->version > 1.0) &&
									    !uh_cgi_header_lookup(res, "Transfer-Encoding")
									) {
										uh_http_send(cl, NULL,
											"Transfer-Encoding: chunked\r\n", -1);
									}

									/* write headers from CGI program */
									foreach_header(i, res->headers)
									{
										uh_http_sendf(cl, NULL, "%s: %s\r\n",
											res->headers[i], res->headers[i+1]);
									}

									/* terminate header */
									uh_http_send(cl, NULL, "\r\n", -1);

									/* push out remaining head buffer */
									if( hdroff < hdrlen )
										uh_http_send(cl, req, &hdr[hdroff], hdrlen - hdroff);
								}

								/* ... failed and head buffer exceeded */
								else if( hdrlen >= sizeof(hdr) )
								{
									uh_cgi_error_500(cl, req,
										"The CGI program generated an invalid response:\n\n");

									uh_http_send(cl, req, hdr, hdrlen);
								}

								/* ... failed but free buffer space, try again */
								else
								{
									continue;
								}

								/* push out remaining read buffer */
								if( bufoff < buflen )
									uh_http_send(cl, req, &buf[bufoff], buflen - bufoff);

								header_sent = 1;
								continue;
							}


							/* headers complete, pass through buffer to socket */
							uh_http_send(cl, req, buf, buflen);
						}

						/* looks like eof from child */
						else
						{
							/* send final chunk if we're in chunked transfer mode */
							uh_http_send(cl, req, "", 0);
							break;
						}
					}
				}

				/* no activity for 3 seconds... looks dead */
				else
				{
					uh_http_sendhf(cl, 504, "Gateway Timeout",
						"The CGI script took too long to produce a response");

					break;
				}
			}

			close(rfd[0]);
			close(wfd[1]);

			break;
	}
}

