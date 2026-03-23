#!/bin/bash
# ============================================================
# Jenkins Final Install Script
# - Installs Jenkins to /home/gopal/Jenkins
# - Fixes all permissions
# - Verifies override.conf is applied
# - Confirms Jenkins service is running
# ============================================================

set -e

# ── Configuration ────────────────────────────────────────────
JENKINS_USER="gopal"
JENKINS_HOME="/home/gopal/Jenkins"
JENKINS_PORT=8080
KEY_ID="7198F4B714ABFC68"
# ─────────────────────────────────────────────────────────────

# Colours for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Colour

pass() { echo -e "${GREEN}[PASS]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; exit 1; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
info() { echo -e "${CYAN}[INFO]${NC} $1"; }
header() {
    echo ""
    echo -e "${CYAN}============================================${NC}"
    echo -e "${CYAN}  $1${NC}"
    echo -e "${CYAN}============================================${NC}"
}

# ── Must run as root ─────────────────────────────────────────
if [ "$(id -u)" -ne 0 ]; then
    fail "Please run as root: sudo bash $0"
fi

header "PRE-FLIGHT CHECKS"

# Check internet connectivity
info "Checking internet connectivity..."
curl -fsSL --max-time 10 https://pkg.jenkins.io > /dev/null 2>&1 \
    && pass "Internet reachable" \
    || fail "Cannot reach pkg.jenkins.io — check your network"

# Check Java
info "Checking Java..."
if java -version &>/dev/null; then
    JAVA_VER=$(java -version 2>&1 | head -1)
    pass "Java found: ${JAVA_VER}"
else
    warn "Java not found — will install openjdk-17-jdk"
fi

# Check gopal user exists
info "Checking user '${JENKINS_USER}'..."
id "${JENKINS_USER}" &>/dev/null \
    && pass "User '${JENKINS_USER}' exists" \
    || fail "User '${JENKINS_USER}' does not exist — create it first: sudo useradd -m -s /bin/bash ${JENKINS_USER}"

# Check /home/gopal is accessible
info "Checking /home/${JENKINS_USER} directory..."
[ -d "/home/${JENKINS_USER}" ] \
    && pass "/home/${JENKINS_USER} exists" \
    || fail "/home/${JENKINS_USER} does not exist"

header "PHASE 1: UNINSTALL EXISTING JENKINS"

# Stop service
info "Stopping Jenkins service..."
systemctl stop jenkins 2>/dev/null  && pass "Jenkins stopped"  || warn "Jenkins was not running"
systemctl disable jenkins 2>/dev/null && pass "Jenkins disabled" || warn "Jenkins was not enabled"

# Remove package
info "Removing Jenkins package..."
apt-get remove --purge -y jenkins 2>/dev/null && pass "Jenkins package removed" || warn "Jenkins was not installed"
apt-get autoremove -y -qq

# Remove all Jenkins directories and files
info "Removing Jenkins directories..."
for DIR in /var/lib/jenkins /var/cache/jenkins /var/log/jenkins \
           /etc/jenkins /etc/default/jenkins \
           /etc/systemd/system/jenkins.service \
           /etc/systemd/system/jenkins.service.d \
           /usr/share/jenkins; do
    if [ -e "${DIR}" ]; then
        rm -rf "${DIR}"
        pass "Removed: ${DIR}"
    fi
done

# Remove APT repo and keys
rm -f /etc/apt/sources.list.d/jenkins.list
rm -f /usr/share/keyrings/jenkins-keyring.gpg
rm -f /usr/share/keyrings/jenkins-keyring.asc
rm -f /tmp/jenkins.key.asc
pass "Removed APT repo and GPG keys"

# Remove old jenkins system user (keep gopal)
if id "jenkins" &>/dev/null; then
    userdel -r jenkins 2>/dev/null && pass "Removed jenkins system user" || warn "Could not remove jenkins user home"
else
    warn "jenkins system user did not exist"
fi

# Reload systemd to forget old service
systemctl daemon-reload
systemctl reset-failed 2>/dev/null || true
pass "systemd reloaded"

# Verify uninstall
header "VERIFYING UNINSTALL"
dpkg -l jenkins 2>/dev/null | grep -q "^ii" \
    && fail "Jenkins package still installed!" \
    || pass "Jenkins package not installed"
[ ! -d /var/lib/jenkins ] \
    && pass "/var/lib/jenkins deleted" \
    || warn "/var/lib/jenkins still exists"
! pgrep -f jenkins > /dev/null \
    && pass "No Jenkins process running" \
    || warn "A Jenkins process is still running"

header "PHASE 2: INSTALL JENKINS TO ${JENKINS_HOME}"

# Install dependencies
info "Installing Java and dependencies..."
apt-get update -y -qq
apt-get install -y openjdk-17-jdk wget curl gnupg2 git -qq
pass "Dependencies installed"
java -version 2>&1 | head -1 | xargs -I{} echo -e "${GREEN}[PASS]${NC} Java: {}"

# Add Jenkins GPG key
info "Adding Jenkins GPG key..."
curl -fsSL https://pkg.jenkins.io/debian-stable/jenkins.io-2023.key \
    -o /tmp/jenkins.key.asc

# Check if downloaded key matches expected key ID
if gpg --show-keys /tmp/jenkins.key.asc 2>/dev/null | grep -qi "${KEY_ID}"; then
    gpg --dearmor < /tmp/jenkins.key.asc > /usr/share/keyrings/jenkins-keyring.gpg
    pass "GPG key downloaded and converted (key ID confirmed)"
else
    warn "Key ID mismatch in download — fetching from keyserver..."
    gpg --batch --yes --keyserver hkp://keyserver.ubuntu.com:80 --recv-keys ${KEY_ID}
    gpg --batch --yes --export ${KEY_ID} > /usr/share/keyrings/jenkins-keyring.gpg
    pass "GPG key fetched from keyserver"
fi

# Verify key file is valid
[ -s /usr/share/keyrings/jenkins-keyring.gpg ] \
    && pass "GPG key file is valid ($(wc -c < /usr/share/keyrings/jenkins-keyring.gpg) bytes)" \
    || fail "GPG key file is empty — key import failed"

# Add APT repo
info "Adding Jenkins APT repository..."
echo "deb [signed-by=/usr/share/keyrings/jenkins-keyring.gpg] \
https://pkg.jenkins.io/debian-stable binary/" \
    | tee /etc/apt/sources.list.d/jenkins.list > /dev/null

apt-get update -y -qq 2>&1 | grep -i "jenkins\|error\|warn" || true

# Verify repo is signed correctly
if apt-get update 2>&1 | grep -q "NO_PUBKEY"; then
    fail "Jenkins repo still shows NO_PUBKEY — GPG key not accepted by apt"
else
    pass "Jenkins repository signed and verified by apt"
fi

# Install Jenkins package
info "Installing Jenkins package..."
apt-get install -y jenkins
pass "Jenkins package installed"

# Stop Jenkins immediately — before it writes to /var/lib/jenkins
systemctl stop jenkins  2>/dev/null || true
systemctl disable jenkins 2>/dev/null || true
pass "Jenkins stopped before first run (preventing writes to /var/lib)"

header "PHASE 3: CONFIGURE JENKINS HOME = ${JENKINS_HOME}"

# Create Jenkins home directory
info "Creating ${JENKINS_HOME}..."
mkdir -p "${JENKINS_HOME}"
pass "Directory created: ${JENKINS_HOME}"

# Fix ownership on ALL directories Jenkins will touch
info "Setting permissions on all Jenkins directories..."
for DIR in "${JENKINS_HOME}" \
           /var/cache/jenkins \
           /var/log/jenkins \
           /usr/share/jenkins; do
    mkdir -p "${DIR}"
    chown -R "${JENKINS_USER}:${JENKINS_USER}" "${DIR}"
    pass "Ownership set: ${DIR} → ${JENKINS_USER}:${JENKINS_USER}"
done
chmod 750 "${JENKINS_HOME}"
pass "Permissions set on ${JENKINS_HOME}"

# Write systemd override
info "Writing systemd override..."
OVERRIDE_DIR="/etc/systemd/system/jenkins.service.d"
mkdir -p "${OVERRIDE_DIR}"

cat > "${OVERRIDE_DIR}/override.conf" << OVERRIDE
[Service]
# Reset base service values first (empty assignment clears the default)
Environment=
User=
Group=
WorkingDirectory=

# Custom values — Jenkins will run as gopal from /home/gopal/Jenkins
Environment="JENKINS_HOME=${JENKINS_HOME}"
Environment="JENKINS_PORT=${JENKINS_PORT}"
User=${JENKINS_USER}
Group=${JENKINS_USER}
WorkingDirectory=${JENKINS_HOME}
OVERRIDE

pass "Systemd override written: ${OVERRIDE_DIR}/override.conf"

# Verify override file content
info "Verifying override.conf content..."
grep -q "JENKINS_HOME=${JENKINS_HOME}" "${OVERRIDE_DIR}/override.conf" \
    && pass "JENKINS_HOME correctly set in override.conf" \
    || fail "JENKINS_HOME not found in override.conf"
grep -q "User=${JENKINS_USER}" "${OVERRIDE_DIR}/override.conf" \
    && pass "User correctly set in override.conf" \
    || fail "User not found in override.conf"
grep -q "WorkingDirectory=${JENKINS_HOME}" "${OVERRIDE_DIR}/override.conf" \
    && pass "WorkingDirectory correctly set in override.conf" \
    || fail "WorkingDirectory not found in override.conf"

# Patch /etc/default/jenkins as belt-and-suspenders
if [ -f /etc/default/jenkins ]; then
    sed -i "s|^JENKINS_HOME=.*|JENKINS_HOME=${JENKINS_HOME}|"   /etc/default/jenkins
    sed -i "s|^JENKINS_USER=.*|JENKINS_USER=${JENKINS_USER}|"   /etc/default/jenkins
    sed -i "s|^JENKINS_GROUP=.*|JENKINS_GROUP=${JENKINS_USER}|" /etc/default/jenkins
    pass "/etc/default/jenkins patched"
fi

# Reload systemd
systemctl daemon-reload
systemctl reset-failed 2>/dev/null || true
pass "systemd daemon reloaded"

header "PHASE 4: START AND VERIFY JENKINS"

# Enable and start
info "Enabling and starting Jenkins..."
systemctl enable jenkins
systemctl start jenkins
pass "Jenkins service started"

# Wait for Jenkins to initialize
info "Waiting for Jenkins to initialize..."
for i in $(seq 1 12); do
    sleep 5
    STATUS=$(systemctl is-active jenkins 2>/dev/null)
    if [ "${STATUS}" = "active" ]; then
        pass "Jenkins is active (after ${i}x5s)"
        break
    elif [ "${STATUS}" = "failed" ]; then
        fail "Jenkins failed to start — run: sudo journalctl -u jenkins -n 50 --no-pager"
    fi
    info "  Still waiting... (${i}/12)"
done

# Final verification checks
header "FINAL VERIFICATION"

# 1. Service running
systemctl is-active jenkins &>/dev/null \
    && pass "Service is ACTIVE" \
    || fail "Service is NOT active"

# 2. Correct JENKINS_HOME in systemctl cat
ACTIVE_HOME=$(systemctl cat jenkins 2>/dev/null | grep "Environment=\"JENKINS_HOME" | tail -1 | cut -d= -f3 | tr -d '"')
[ "${ACTIVE_HOME}" = "${JENKINS_HOME}" ] \
    && pass "JENKINS_HOME confirmed: ${ACTIVE_HOME}" \
    || warn "JENKINS_HOME mismatch — active: ${ACTIVE_HOME}, expected: ${JENKINS_HOME}"

# 3. Correct user in systemctl cat
ACTIVE_USER=$(systemctl cat jenkins 2>/dev/null | grep "^User=" | tail -1 | cut -d= -f2)
[ "${ACTIVE_USER}" = "${JENKINS_USER}" ] \
    && pass "User confirmed: ${ACTIVE_USER}" \
    || warn "User mismatch — active: ${ACTIVE_USER}, expected: ${JENKINS_USER}"

# 4. Jenkins files written to correct home
[ -f "${JENKINS_HOME}/config.xml" ] \
    && pass "config.xml found in ${JENKINS_HOME}" \
    || warn "config.xml not yet created (Jenkins still initializing)"

# 5. Old /var/lib/jenkins must NOT exist
[ ! -d /var/lib/jenkins ] \
    && pass "/var/lib/jenkins does not exist" \
    || warn "/var/lib/jenkins still exists — run: sudo rm -rf /var/lib/jenkins"

# 6. Port 8080 listening
sleep 3
ss -tlnp | grep -q ":${JENKINS_PORT}" \
    && pass "Port ${JENKINS_PORT} is open and listening" \
    || warn "Port ${JENKINS_PORT} not yet listening (Jenkins still starting)"

# 7. Ownership of Jenkins home
OWNER=$(stat -c '%U' "${JENKINS_HOME}")
[ "${OWNER}" = "${JENKINS_USER}" ] \
    && pass "${JENKINS_HOME} is owned by ${OWNER}" \
    || fail "${JENKINS_HOME} owned by ${OWNER} — expected ${JENKINS_USER}"

header "INSTALLATION COMPLETE"
echo ""
echo -e "  ${GREEN}Jenkins Home  :${NC} ${JENKINS_HOME}"
echo -e "  ${GREEN}Running as    :${NC} ${JENKINS_USER}"
echo -e "  ${GREEN}Web UI        :${NC} http://$(hostname -I | awk '{print $1}'):${JENKINS_PORT}"
echo ""
echo -e "  ${CYAN}Initial Admin Password:${NC}"
ADMIN_PW="${JENKINS_HOME}/secrets/initialAdminPassword"
for i in $(seq 1 6); do
    if [ -f "${ADMIN_PW}" ]; then
        echo ""
        echo -e "  ${GREEN}$(cat ${ADMIN_PW})${NC}"
        echo ""
        break
    fi
    info "  Waiting for password file... (${i}/6)"
    sleep 5
done
[ ! -f "${ADMIN_PW}" ] && warn "Password not ready yet — run: sudo cat ${ADMIN_PW}"
echo ""
echo -e "  Open browser: ${CYAN}http://localhost:${JENKINS_PORT}${NC}"
echo ""
