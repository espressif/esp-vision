#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
import json
import os
import re
import subprocess
import sys

import gitlab
import requests
from git import Git
from git import Repo
from gitlab.exceptions import GitlabGetError


GITHUB_TOKEN = os.environ['GITHUB_TOKEN']
GITLAB_URL = os.environ['GITLAB_URL']
GITLAB_TOKEN = os.environ['GITLAB_TOKEN']
DEFAULT_GITLAB_NAMESPACE = 'espressif'
GITLAB_NAMESPACE = os.environ.get('GITLAB_NAMESPACE', DEFAULT_GITLAB_NAMESPACE)

GITHUB_REMOTE = 'origin'
GITLAB_REMOTE = 'gitlab'
URL_HDR_LEN = 8

LABEL_MERGE = 'PR-Sync-Merge'
LABEL_REBASE = 'PR-Sync-Rebase'
LABEL_UPDATE = 'PR-Sync-Update'

CODEOWNERS_CHECK_PATH = './tools/ci/check_codeowners.py'


def pr_check_approver(pr_creator, pr_comments_url, pr_approve_labeller, esp_idf_flow):
    print('Checking PR comment and affixed label...')
    # Requires Github Access Token, with Push Access

    res = requests.get(pr_comments_url, headers={'Authorization': 'token ' + GITHUB_TOKEN}, timeout=30)
    r_data = res.json()

    for comment in reversed(r_data):
        comment_body = comment['body']

        if (
            bool(re.match('sha=', comment_body, re.I)) and comment['user']['login'] == pr_approve_labeller
            and (esp_idf_flow is False or pr_approve_labeller != pr_creator) # allow to be creator and approver the same person in non-esp-idf flow
        ):
            return comment_body[4:]

    raise RuntimeError('PR Comment Error: Ensure that Command comment exists and PR commenter and labeller match!')


def pr_check_forbidden_files(pr_files_url):
    print('Checking if PR modified forbidden files...')
    # Requires Github Access Token, with Push Access

    res = requests.get(pr_files_url, headers={'Authorization': 'token ' + GITHUB_TOKEN}, timeout=30)
    r_data = res.json()

    pr_files = [file_info['filename'] for file_info in r_data if (file_info['filename']).find('.gitlab') != -1 or (file_info['filename']).find('.github') != -1]
    if pr_files:
        raise RuntimeError('PR modifying forbidden files!!!')


def setup_project(repo_fullname, pr_base_branch):
    print('Connecting to GitLab...')
    gl = gitlab.Gitlab(url=GITLAB_URL, private_token=GITLAB_TOKEN)
    gl.auth()
    project_name = repo_fullname.split('/')[-1]
    gl_project_url = f'{GITLAB_URL[:URL_HDR_LEN]}{GITLAB_TOKEN}:{GITLAB_TOKEN}@{GITLAB_URL[URL_HDR_LEN:]}/{GITLAB_NAMESPACE}/{project_name}.git'

    git = Git('.')

    print('Adding and fetching the internal remote...')
    git.remote('add', GITLAB_REMOTE, gl_project_url)
    git.pull(GITLAB_REMOTE, pr_base_branch)

    return gl


def check_update_label(pr_labels_list):
    label_validity = [label['name'] for label in pr_labels_list if label['name'] == LABEL_MERGE or label['name'] == LABEL_REBASE]

    if not label_validity:
        raise RuntimeError('PR-Sync-Update Label: Illegal use!')


# Update existing MR
def update_mr(pr_num, pr_head_branch, pr_commit_id, project_gl):
    try:
        project_gl.branches.get(pr_head_branch)
    except Exception as exc:
        raise RuntimeError('PR Update: No branch found on internal remote to update!') from exc

    git = Git('.')

    print('Updating the PR branch...')
    git.fetch(GITHUB_REMOTE, f'pull/{str(pr_num)}/head')
    git.checkout('FETCH_HEAD', b=pr_head_branch)

    print('Checking whether specified commit ID matches with user branch HEAD...')
    expected_commit_id = git.rev_parse('--short', 'HEAD')

    if not pr_commit_id.startswith(expected_commit_id):
        raise RuntimeError('PR Commit SHA1 in workflow comment and user branch do not match!')

    print('Pushing to remote...')
    git.push('--force', GITLAB_REMOTE, pr_head_branch)


# Merge PRs with/without Rebase
def sync_pr(pr_num, pr_head_branch, pr_commit_id, project_gl, pr_base_branch, pr_html_url, rebase_flag):  # pylint: disable=too-many-arguments
    try:
        project_gl.branches.get(pr_head_branch)
    except GitlabGetError:
        pass
    else:
        raise RuntimeError('PR Merge/Rebase: Branch/MR already exists for PR!')

    git = Git('.')

    print('Fetching the PR branch...')
    git.fetch(GITHUB_REMOTE, 'pull/' + str(pr_num) + '/head')

    print('Checking out the PR branch...')
    git.checkout('FETCH_HEAD', b=pr_head_branch)

    print('Checking whether specified commit ID matches with user branch HEAD...')
    expected_commit_id = git.rev_parse('--short', 'HEAD')

    if not pr_commit_id.startswith(expected_commit_id):
        raise RuntimeError('PR Commit SHA1 in workflow comment and user branch do not match!')

    if rebase_flag:
        repo = Repo('.')
        repo.config_writer().set_value('user', 'name', os.environ['GIT_CONFIG_NAME']).release()
        repo.config_writer().set_value('user', 'email', os.environ['GIT_CONFIG_EMAIL']).release()

        print(f'Rebasing with the latest {pr_base_branch} branch...')
        git.rebase(pr_base_branch)

        commit = repo.head.commit
        new_cmt_msg = f'{commit.message}\n\nCloses {pr_html_url}'

        print('Amending commit message (Adding additional info about commit)...')
        git.execute(['git', 'commit', '--amend', '-m', new_cmt_msg])

    print('Pushing to remote...')
    git.push('--set-upstream', GITLAB_REMOTE, pr_head_branch)


def notify_maintainers(pr_head_branch, pr_base_branch, project_gl, mr_iid):
    git = Git('.')

    commits_ahead = git.execute(['git', 'rev-list', '--left-right', '--count', f'{pr_base_branch}..{pr_head_branch}']).split('\t')[1]
    modified_files = git.execute(['git', 'diff-tree', '--no-commit-id', '--name-only', '-r', f'HEAD..HEAD~{commits_ahead}']).splitlines()

    codeowners_list = []
    for file in modified_files:
        cmd = f'{sys.executable} {CODEOWNERS_CHECK_PATH} identify {file}'
        try:
            output = subprocess.check_output(cmd, shell=True, text=True, stderr=subprocess.STDOUT)
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(f'Command failed {exc.returncode} {exc.output}') from exc
        codeowners_list.extend(output.splitlines())

    codeowners_list = list(set(filter(None, codeowners_list)))
    owners_to_be_notified = ' '.join(codeowners_list)

    print('Notifying relevant users...')
    resource = project_gl.mergerequests.get(mr_iid)
    resource.discussions.create({'body': f'{owners_to_be_notified}: FYI'})


def main():
    if 'GITHUB_REPOSITORY' not in os.environ:
        print('Not running in GitHub action context, nothing to do')
        return

    if GITLAB_NAMESPACE == DEFAULT_GITLAB_NAMESPACE:
        github_repo_namespace = os.environ['GITHUB_REPOSITORY'].split('/')[0]
        if  github_repo_namespace != DEFAULT_GITLAB_NAMESPACE:
            print(f'Cannot sync {github_repo_namespace} namespace repo to {DEFAULT_GITLAB_NAMESPACE} GitLab repo!')
            return

    # The path of the file with the complete webhook event payload. For example, /github/workflow/event.json.
    with open(os.environ['GITHUB_EVENT_PATH'], 'r', encoding='utf-8') as file:
        event = json.load(file)

    repo_fullname = event['repository']['full_name']

    is_esp_idf = repo_fullname == 'espressif/esp-idf'

    pr_label = event['label']['name']
    pr_labels_list = event['pull_request']['labels']

    pr_approve_labeller = event['sender']['login']
    pr_creator = event['pull_request']['user']['login']
    pr_comments_url = event['pull_request']['comments_url']
    # Checks whether the approve labeller and workflow initiator are the same
    pr_commit_id = pr_check_approver(pr_creator, pr_comments_url, pr_approve_labeller, is_esp_idf)

    pr_num = event['pull_request']['number']
    pr_head_branch = 'contrib/github_pr_' + str(pr_num)
    pr_rest_url = event['pull_request']['url']
    pr_html_url = event['pull_request']['html_url']
    pr_base_branch = event['pull_request']['base']['ref']

    pr_files_url = pr_rest_url + '/files'
    # Check whether the PR has modified forbidden files
    pr_check_forbidden_files(pr_files_url)

    # Getting the PR title and body
    pr_title = event['pull_request']['title']
    pr_title_desc = pr_title + ' (GitHub PR)'
    pr_body = event['pull_request']['body'] or ''

    # Gitlab setup and cloning internal codebase
    gl = setup_project(repo_fullname, pr_base_branch)
    project_gl = gl.projects.get(f'{GITLAB_NAMESPACE}/{repo_fullname.split("/")[-1]}')

    if pr_label == LABEL_REBASE:
        sync_pr(pr_num, pr_head_branch, pr_commit_id, project_gl, pr_base_branch, pr_html_url, rebase_flag=True)
    elif pr_label == LABEL_MERGE:
        sync_pr(pr_num, pr_head_branch, pr_commit_id, project_gl, pr_base_branch, pr_html_url, rebase_flag=False)
    elif pr_label == LABEL_UPDATE:
        check_update_label(pr_labels_list)
        update_mr(pr_num, pr_head_branch, pr_commit_id, project_gl)
        print('Done with the workflow!')
        return
    else:
        raise RuntimeError('Illegal program flow!')

    print('Creating a merge request...')
    mr = project_gl.mergerequests.create(
        {
            'source_branch': pr_head_branch,
            'target_branch': pr_base_branch,
            'title': pr_title_desc,
            'remove_source_branch': True,
        }
    )

    print('Updating merge request description...')
    mr_desc = '## Description \n' + pr_body + '\n ##### (Add more info here)' + '\n## Related'
    mr_desc += '\n* Merges ' + pr_html_url
    if is_esp_idf:
        mr_desc += (
            '\n## Release notes (Mandatory)\n* [component/development area] <Please update release notes, do NOT remove GitHub PR pointer> (' + pr_html_url + ')'
        )

    mr.description = mr_desc
    mr.save()

    if is_esp_idf:
        notify_maintainers(pr_head_branch, pr_base_branch, project_gl, mr.iid)

    print('Done with the workflow!')


if __name__ == '__main__':
    main()
