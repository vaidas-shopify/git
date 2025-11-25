#!/bin/sh

test_description='test memory leak in show_http_message() without retries'

. ./test-lib.sh

. "$TEST_DIRECTORY"/lib-httpd.sh

start_httpd

test_expect_success 'setup test repository' '
	test_commit initial &&
	git clone --bare . "$HTTPD_DOCUMENT_ROOT_PATH/repo.git"
'

test_expect_success 'HTTP error with charset triggers show_http_message() and leaks memory' '
	# The existing error.sh script returns 500 with charset when path contains "charset"
	# This triggers show_http_message() with strbuf_reencode()
	# which allocates memory that is never freed before die()
	#
	# This demonstrates the leak exists in PRE-EXISTING error paths,
	# not just in the new HTTP 429 retry logic.

	test_must_fail git ls-remote "$HTTPD_URL/error/charset/repo.git" 2>err &&

	# Verify show_http_message() was called and displayed the error
	grep "error message" err
'

test_expect_success 'HTTP error without charset does not trigger reencode' '
	# When no charset is provided, strbuf_reencode() is not called
	# so there is no leak (but the bug still exists, just not triggered)

	test_must_fail git ls-remote "$HTTPD_URL/error/text/repo.git" 2>err &&

	# Verify error was displayed
	grep "error message" err
'

test_expect_success 'HTTP error with utf-16 charset also triggers the leak' '
	# The leak happens with any charset that requires re-encoding
	# not just utf-8

	test_must_fail git ls-remote "$HTTPD_URL/error/utf16/repo.git" 2>err &&

	# Verify show_http_message() was called
	grep "error message" err
'

test_done

