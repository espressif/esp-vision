FROM python:3.11-alpine3.18

ENV LANG C.UTF-8
ENV LC_ALL C.UTF-8

# Install GIT
RUN apk add --no-cache git

# Upgrade pip
RUN pip install --no-cache-dir --upgrade pip

# Install Python dependencies
COPY requirements.txt /tmp/requirements.txt
RUN pip install --no-cache-dir -r /tmp/requirements.txt

# Mark the `/github/workspace` directory as "safe" for Git operations.
# This is necessary to avoid security restrictions introduced in newer versions of Git,
# especially in CI environments like GitHub Actions, where Git commands are run in the `/github/workspace`.
# Without this configuration, Git might refuse to operate in the workspace directory due to security policies.
RUN git config --system --add safe.directory /github/workspace
RUN git config --global pull.ff only

COPY sync_pr_to_gitlab/github_pr_to_internal_pr.py /

ENTRYPOINT ["python", "/github_pr_to_internal_pr.py"]
