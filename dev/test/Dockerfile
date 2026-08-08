FROM debian:trixie-slim

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    dovecot-imapd \
 && rm -rf /var/lib/apt/lists/*

RUN useradd -m test && echo 'test:test' | chpasswd \
 && useradd -m test2 && echo 'test2:test2' | chpasswd

# Dovecot 2.4 (trixie) has a new config system; replace the stock
# config with a complete minimal one rather than sed-patching it.
RUN rm -rf /etc/dovecot/conf.d \
 && printf '%s\n' \
    'dovecot_config_version = 2.4.1' \
    'dovecot_storage_version = 2.4.1' \
    'protocols = imap' \
    'listen = *' \
    'ssl = no' \
    'auth_allow_cleartext = yes' \
    'auth_mechanisms = plain login' \
    'mail_driver = maildir' \
    'mail_path = ~/Maildir' \
    'passdb pam {' \
    '  driver = pam' \
    '}' \
    'userdb passwd {' \
    '  driver = passwd' \
    '}' \
    > /etc/dovecot/dovecot.conf

COPY seed/ /seed/

# INBOX: 9 seen (cur/ with :2,S maildir flag suffix) + 3 unseen (new/).
# Dev folder: 3 seen. The S suffix is required — a flagless cur/ file
# reports as unseen over IMAP.
RUN mkdir -p /home/test/Maildir/cur /home/test/Maildir/new /home/test/Maildir/tmp \
 && mkdir -p /home/test/Maildir/.Dev/cur /home/test/Maildir/.Dev/new /home/test/Maildir/.Dev/tmp \
 && for f in /seed/inbox-seen/*.eml; do \
      mv "$f" "/home/test/Maildir/cur/$(basename "$f" .eml).eml:2,S"; done \
 && cp /seed/inbox-unseen/*.eml /home/test/Maildir/new/ \
 && for f in /seed/dev/*.eml; do \
      mv "$f" "/home/test/Maildir/.Dev/cur/$(basename "$f" .eml).eml:2,S"; done \
 && chown -R test:test /home/test/Maildir

# test2 account: 2 seen in INBOX + 2 seen in Alerts folder.
RUN mkdir -p /home/test2/Maildir/cur /home/test2/Maildir/new /home/test2/Maildir/tmp \
 && mkdir -p /home/test2/Maildir/.Alerts/cur /home/test2/Maildir/.Alerts/new /home/test2/Maildir/.Alerts/tmp \
 && for f in /seed/test2-inbox/*.eml; do \
      mv "$f" "/home/test2/Maildir/cur/$(basename "$f" .eml).eml:2,S"; done \
 && for f in /seed/test2-alerts/*.eml; do \
      mv "$f" "/home/test2/Maildir/.Alerts/cur/$(basename "$f" .eml).eml:2,S"; done \
 && chown -R test2:test2 /home/test2/Maildir

EXPOSE 143
CMD ["dovecot", "-F"]
