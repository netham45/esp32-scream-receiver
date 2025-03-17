// Use current SSID value from template
const currentSsid = '{{CURRENT_SSID}}' !== 'Not configured' ? '{{CURRENT_SSID}}' : '';
const isConnected = currentSsid && currentSsid !== 'Not configured';

// Set initial status styling
document.addEventListener('DOMContentLoaded', function() {
    updateStatusStyles('{{CONNECTION_STATUS}}');
    // Don't scan automatically on page load, wait for user to click scan button
    loadSettings();
    
    // Show initial message in network list
    document.getElementById('network-list').innerHTML = '<div style="padding: 15px; text-align: center;">Click "Scan" to search for WiFi networks</div>';
});

function openTab(evt, tabName) {
    // Hide all tab content
    const tabContents = document.getElementsByClassName('tab-content');
    for (let i = 0; i < tabContents.length; i++) {
        tabContents[i].classList.remove('active');
    }
    
    // Remove active class from all tabs
    const tabs = document.getElementsByClassName('tab');
    for (let i = 0; i < tabs.length; i++) {
        tabs[i].classList.remove('active');
    }
    
    // Show the specific tab content and add active class to the button
    document.getElementById(tabName).classList.add('active');
    evt.currentTarget.classList.add('active');
}

function loadSettings() {
    fetch('/api/settings')
        .then(response => {
            if (!response.ok) {
                throw new Error('Failed to load settings');
            }
            return response.json();
        })
        .then(settings => {
            // Network settings
            document.getElementById('port').value = settings.port;
            document.getElementById('ap_ssid').value = settings.ap_ssid || '';
            document.getElementById('ap_password').value = settings.ap_password || '';
            document.getElementById('hide_ap_when_connected').checked = settings.hide_ap_when_connected;
            
            // Buffer settings
            document.getElementById('initial_buffer_size').value = settings.initial_buffer_size;
            document.getElementById('buffer_grow_step_size').value = settings.buffer_grow_step_size;
            document.getElementById('max_buffer_size').value = settings.max_buffer_size;
            document.getElementById('max_grow_size').value = settings.max_grow_size;
            
            // Audio settings
            document.getElementById('sample_rate').value = settings.sample_rate;
            document.getElementById('bit_depth').value = settings.bit_depth;
            document.getElementById('volume').value = settings.volume;
            
            // SPDIF settings (only if element exists)
            if (document.getElementById('spdif_data_pin') && settings.spdif_data_pin !== undefined) {
                document.getElementById('spdif_data_pin').value = settings.spdif_data_pin;
            }
            
            // Sleep settings
            document.getElementById('silence_threshold_ms').value = settings.silence_threshold_ms;
            document.getElementById('network_check_interval_ms').value = settings.network_check_interval_ms;
            document.getElementById('activity_threshold_packets').value = settings.activity_threshold_packets;
            document.getElementById('silence_amplitude_threshold').value = settings.silence_amplitude_threshold;
            document.getElementById('network_inactivity_timeout_ms').value = settings.network_inactivity_timeout_ms;
            
            showSettingsSuccess('Settings loaded successfully');
        })
        .catch(error => {
            showSettingsAlert('Failed to load settings: ' + error.message);
        });
}

function saveSettings(event) {
    event.preventDefault();
    
    const form = document.getElementById('settings-form');
    const formData = new FormData(form);
    const settings = {};
    
    // Convert form data to JSON object
    for (let [key, value] of formData.entries()) {
        // Convert numeric values
        if (!isNaN(value) && key !== 'ap_password') {
            if (key === 'volume') {
                settings[key] = parseFloat(value);
            } else {
                settings[key] = parseInt(value, 10);
            }
        } else {
            settings[key] = value;
        }
    }
    
    // Handle checkbox values (checkboxes are only included in formData when checked)
    settings.hide_ap_when_connected = document.getElementById('hide_ap_when_connected').checked;
    
    fetch('/api/settings', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
        },
        body: JSON.stringify(settings),
    })
        .then(response => {
            if (!response.ok) {
                throw new Error('Failed to save settings');
            }
            return response.json();
        })
        .then(result => {
            showSettingsSuccess('Settings saved successfully');
        })
        .catch(error => {
            showSettingsAlert('Failed to save settings: ' + error.message);
        });
}

function showSettingsAlert(message) {
    const alert = document.getElementById('settings-alert');
    alert.textContent = message;
    alert.style.display = 'block';
    
    // Hide success message if it's visible
    document.getElementById('settings-success').style.display = 'none';
    
    // Auto-hide after 5 seconds
    setTimeout(() => {
        alert.style.display = 'none';
    }, 5000);
}

function showSettingsSuccess(message) {
    const success = document.getElementById('settings-success');
    success.textContent = message;
    success.style.display = 'block';
    
    // Hide alert message if it's visible
    document.getElementById('settings-alert').style.display = 'none';
    
    // Auto-hide after 5 seconds
    setTimeout(() => {
        success.style.display = 'none';
    }, 5000);
}

function updateStatusStyles(status) {
    const statusEl = document.getElementById('connection-status');
    
    // Remove all status classes
    statusEl.classList.remove('status-connected', 'status-connecting', 'status-failed', 'status-ap');
    
    // Add appropriate class based on status
    if (status === 'Connected') {
        statusEl.classList.add('status-connected');
    } else if (status === 'Connecting...') {
        statusEl.classList.add('status-connecting');
    } else if (status === 'Connection failed') {
        statusEl.classList.add('status-failed');
    } else if (status === 'Access Point Mode') {
        statusEl.classList.add('status-ap');
    }
}

function togglePassword() {
    const passwordInput = document.getElementById('password');
    const toggleButton = document.querySelector('.password-toggle');
    
    if (passwordInput.type === 'password') {
        passwordInput.type = 'text';
        toggleButton.textContent = 'Hide';
    } else {
        passwordInput.type = 'password';
        toggleButton.textContent = 'Show';
    }
}

function startScan(event) {
    if (event) event.preventDefault();
    
    document.getElementById('scanning-info').style.display = 'block';
    document.getElementById('network-list').innerHTML = '';
    
    fetch('/scan')
        .then(response => {
            if (!response.ok) {
                throw new Error('Network scan failed');
            }
            return response.json();
        })
        .then(networks => {
            document.getElementById('scanning-info').style.display = 'none';
            const networkListEl = document.getElementById('network-list');
            
            if (networks.length === 0) {
                networkListEl.innerHTML = '<div style="padding: 15px; text-align: center;">No networks found</div>';
                return;
            }
            
            // Sort networks by signal strength
            networks.sort((a, b) => b.rssi - a.rssi);
            
            networks.forEach(network => {
                const item = document.createElement('div');
                item.className = 'network-item';
                
                const nameSpan = document.createElement('span');
                nameSpan.className = network.secure ? 'lock-icon' : '';
                nameSpan.textContent = network.ssid;
                
                const signalSpan = document.createElement('span');
                signalSpan.className = 'signal-strength';
                signalSpan.textContent = `${network.rssi} dBm`;
                
                item.appendChild(nameSpan);
                item.appendChild(signalSpan);
                
                item.addEventListener('click', () => {
                    document.getElementById('ssid').value = network.ssid;
                    document.getElementById('password').focus();
                });
                
                networkListEl.appendChild(item);
            });
        })
        .catch(error => {
            document.getElementById('scanning-info').style.display = 'none';
            const networkListEl = document.getElementById('network-list');
            networkListEl.innerHTML = `<div style="padding: 15px; text-align: center; color: #e74c3c;">Error: ${error.message}</div>`;
        });
}

function saveConfiguration(event) {
    event.preventDefault();
    
    const ssid = document.getElementById('ssid').value.trim();
    const password = document.getElementById('password').value;
    
    if (!ssid) { 
        showAlert('Please enter or select a WiFi network');
        return;
    }
    
    // Create form data instead of JSON
    const formData = new URLSearchParams();
    formData.append('ssid', ssid);
    formData.append('password', password);
    
    document.getElementById('alert-message').style.display = 'none';
    document.getElementById('success-message').style.display = 'none';
    
    fetch('/connect', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/x-www-form-urlencoded',
        },
        body: formData.toString(),
    })
        .then(response => {
            if (!response.ok) {
                throw new Error('Failed to connect');
            }
            return response.text(); // Server returns plain text, not JSON
        })
        .then(result => {
            showSuccess('Connecting to WiFi network...');
            document.getElementById('connection-status').textContent = 'Connecting...';
            updateStatusStyles('Connecting...');
            
            // Show status check container after a delay
            setTimeout(() => {
                document.getElementById('status-check-container').style.display = 'block';
            }, 5000);
        })
        .catch(error => {
            showAlert('Failed to connect: ' + error.message);
        });
}

function forgetNetwork() {
    if (!confirm('Are you sure you want to reset all WiFi settings? This will reboot the device into Access Point mode.')) {
        return;
    }
    
    fetch('/forget', { method: 'POST' })
        .then(response => {
            if (!response.ok) {
                throw new Error('Failed to reset WiFi settings');
            }
            return response.json();
        })
        .then(result => {
            showSuccess('WiFi settings reset. Device will reboot into Access Point mode shortly.');
        })
        .catch(error => {
            showAlert('Failed to reset WiFi settings: ' + error.message);
        });
}

function showAlert(message) {
    const alert = document.getElementById('alert-message');
    alert.textContent = message;
    alert.style.display = 'block';
    
    // Hide success message if it's visible
    document.getElementById('success-message').style.display = 'none';
}

function showSuccess(message) {
    const success = document.getElementById('success-message');
    success.textContent = message;
    success.style.display = 'block';
    
    // Hide alert message if it's visible
    document.getElementById('alert-message').style.display = 'none';
}

function checkConnectionStatus() {
    fetch('/status')
        .then(response => {
            if (!response.ok) {
                throw new Error('Failed to check status');
            }
            return response.json();
        })
        .then(status => {
            document.getElementById('connection-status').textContent = status.status;
            updateStatusStyles(status.status);
            
            if (status.connected) {
                showSuccess('Connected successfully! Redirecting...');
                setTimeout(() => {
                    window.location.reload();
                }, 2000);
            } else if (status.status === 'Connection failed') {
                showAlert('Failed to connect to the network. Please check your credentials and try again.');
                document.getElementById('status-check-container').style.display = 'none';
            } else {
                // Still connecting, keep the status check container visible
            }
        })
        .catch(error => {
            showAlert('Failed to check status: ' + error.message);
        });
}
