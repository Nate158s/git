#!/bin/sh

test_description='test disabling of remote-helper paths in clone/fetch'
. ./test-lib.sh
. "$TEST_DIRECTORY/lib-proto-disable.sh"

setup_ext_wrapper

test_expect_success 'setup repository to clone' '
	test_commit one &&
	git branch -M main &&
	mkdir remote &&
	git init --bare -b main remote/repo.git &&
	git push remote/repo.git HEAD
'

test_proto "remote-helper" ext "ext::fake-remote %S repo.git"

test_done
