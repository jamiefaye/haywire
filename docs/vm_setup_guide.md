# VM Setup Guide for Haywire

## Quick Setup for Virgin Ubuntu ARM64 VM

### 1. Initial VM Boot
```bash
# Start VM from scripts directory
cd scripts
./launch_ubuntu_highmem_off.sh
```

### 2. Initial SSH Connection (Password-based)
First connection will use password authentication:
```bash
# From Mac terminal
ssh jff@localhost -p 2222
# Password: p
```

### 3. Create Ubuntu User and Set Up Passwordless SSH

#### On the VM (as jff user):
```bash
# Create ubuntu user if it doesn't exist
sudo useradd -m -s /bin/bash -G sudo ubuntu

# Create SSH directory structure
sudo mkdir -p /home/ubuntu/.ssh
sudo chmod 700 /home/ubuntu/.ssh
sudo chown ubuntu:ubuntu /home/ubuntu/.ssh

# Ensure SSH allows public key authentication
echo "PubkeyAuthentication yes" | sudo tee -a /etc/ssh/sshd_config
sudo systemctl restart ssh
```

#### On your Mac:
```bash
# Generate SSH key if you don't have one
[ -f ~/.ssh/id_rsa ] || ssh-keygen -t rsa -N "" -f ~/.ssh/id_rsa

# Display your public key
cat ~/.ssh/id_rsa.pub
```

#### Back on the VM (as jff):
```bash
# IMPORTANT: Use nano to avoid copy/paste issues with line wrapping
sudo nano /home/ubuntu/.ssh/authorized_keys

# In nano:
# 1. Paste your SSH public key (make Terminal window VERY wide first to avoid wrapping)
# 2. Ensure it's ONE line with NO spaces in the middle of the key
# 3. Format: ssh-rsa [LONG_KEY_STRING] email@domain.com
# 4. Ctrl+O to save, Enter, Ctrl+X to exit

# Fix permissions (SSH is strict about these)
sudo chown ubuntu:ubuntu /home/ubuntu/.ssh/authorized_keys
sudo chmod 600 /home/ubuntu/.ssh/authorized_keys
sudo chmod 755 /home/ubuntu

# Verify the key is correct (should show 1 line, ~400 bytes)
sudo wc -l /home/ubuntu/.ssh/authorized_keys
sudo wc -c /home/ubuntu/.ssh/authorized_keys
```

### 4. Configure SSH Client on Mac

Add to `~/.ssh/config`:
```
Host vm
    HostName localhost
    Port 2222
    User ubuntu
    PreferredAuthentications publickey
    StrictHostKeyChecking no
    IdentityFile ~/.ssh/id_rsa
    IdentitiesOnly yes
```

### 5. Test Passwordless SSH
```bash
# From Mac terminal - should NOT ask for password
ssh vm whoami
# Should output: ubuntu
```

### 6. Deploy Haywire Components

Once passwordless SSH is working:

```bash
# Copy files to VM
scp src/beacon_scanner.cpp vm:/tmp/
scp src/beacon_client.cpp vm:/tmp/

# Compile on VM
ssh vm "cd /tmp && g++ -o beacon_scanner beacon_scanner.cpp"
ssh vm "cd /tmp && g++ -o beacon_client beacon_client.cpp"
```

## Troubleshooting

### SSH Key Not Working?

1. **Check for spaces in the key**:
   - The base64 part should have NO spaces
   - Wrong: `...Ebq+41 6owk177SWr...` (space in middle)
   - Right: `...Ebq+416owk177SWr...` (continuous)

2. **Fix key formatting**:
   ```bash
   # On VM, check the key
   sudo cat /home/ubuntu/.ssh/authorized_keys
   
   # If broken across lines or has spaces, fix with nano
   sudo nano /home/ubuntu/.ssh/authorized_keys
   # Delete everything, paste correctly, save
   ```

3. **Check permissions**:
   ```bash
   sudo ls -la /home/ubuntu/.ssh/
   # Should show:
   # drwx------ (700) for .ssh directory
   # -rw------- (600) for authorized_keys
   ```

4. **Check SSH service**:
   ```bash
   # Ensure public key auth is enabled
   sudo grep PubkeyAuthentication /etc/ssh/sshd_config
   # Should show: PubkeyAuthentication yes
   
   # Restart if needed
   sudo systemctl restart ssh
   ```

### Terminal Copy/Paste Issues

**Problem**: macOS Terminal wraps long lines and adds spaces/newlines when copying

**Solutions**:
1. Make Terminal window VERY wide before copying (wider than the text)
2. Use `pbcopy` on Mac: `cat ~/.ssh/id_rsa.pub | pbcopy`
3. Use nano or vi on VM to paste (they handle long lines better)
4. Check Terminal → Preferences → Profiles → Advanced for wrap settings

### VM Network Issues

If SSH connection refused:
```bash
# Check if VM is running
ps aux | grep qemu

# Check if SSH port is forwarded
netstat -an | grep 2222

# Restart VM if needed
```

## Notes

- Default Ubuntu password is not set (we use SSH keys)
- The `jff` user with password `p` is for emergency access
- Always use `ubuntu` user for normal operations
- The VM memory file is at `/tmp/haywire-vm-mem`