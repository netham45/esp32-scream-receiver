#pragma once

// Simple HTML content for WiFi configuration page (as C string literal)
const char html_config_page[] = 
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"    <meta charset=\"UTF-8\">\n"
"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"    <title>ESP32 WiFi Setup</title>\n"
"    <style>\n"
"        :root {\n"
"            --primary-color: #2c3e50;\n"
"            --accent-color: #3498db;\n"
"            --success-color: #2ecc71;\n"
"            --warning-color: #f39c12;\n"
"            --danger-color: #e74c3c;\n"
"            --light-bg: #f5f7fa;\n"
"            --card-bg: #ffffff;\n"
"            --text-color: #2c3e50;\n"
"            --border-radius: 8px;\n"
"            --shadow: 0 4px 12px rgba(0,0,0,0.1);\n"
"        }\n"
"        body {\n"
"            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;\n"
"            margin: 0;\n"
"            padding: 20px;\n"
"            background: var(--light-bg);\n"
"            color: var(--text-color);\n"
"            line-height: 1.6;\n"
"        }\n"
"        .container {\n"
"            max-width: 480px;\n"
"            margin: 0 auto;\n"
"            background: var(--card-bg);\n"
"            padding: 25px;\n"
"            border-radius: var(--border-radius);\n"
"            box-shadow: var(--shadow);\n"
"        }\n"
"        h1 {\n"
"            color: var(--primary-color);\n"
"            margin-top: 0;\n"
"            font-size: 24px;\n"
"            border-bottom: 2px solid var(--accent-color);\n"
"            padding-bottom: 10px;\n"
"            margin-bottom: 20px;\n"
"        }\n"
"        label {\n"
"            display: block;\n"
"            margin-top: 15px;\n"
"            font-weight: 600;\n"
"            color: var(--primary-color);\n"
"        }\n"
"        input[type=text], input[type=password] {\n"
"            width: 100%;\n"
"            padding: 12px;\n"
"            margin-top: 8px;\n"
"            border: 1px solid #ddd;\n"
"            border-radius: 4px;\n"
"            box-sizing: border-box;\n"
"            transition: border-color 0.3s;\n"
"            font-size: 16px;\n"
"        }\n"
"        input[type=text]:focus, input[type=password]:focus {\n"
"            border-color: var(--accent-color);\n"
"            outline: none;\n"
"            box-shadow: 0 0 0 2px rgba(52, 152, 219, 0.25);\n"
"        }\n"
"        .password-field {\n"
"            position: relative;\n"
"        }\n"
"        .password-toggle {\n"
"            position: absolute;\n"
"            right: 12px;\n"
"            top: 20px;\n"
"            cursor: pointer;\n"
"            color: #7f8c8d;\n"
"            user-select: none;\n"
"            font-size: 14px;\n"
"        }\n"
"        button {\n"
"            background: var(--accent-color);\n"
"            color: white;\n"
"            border: none;\n"
"            padding: 12px 20px;\n"
"            margin-top: 20px;\n"
"            border-radius: 4px;\n"
"            cursor: pointer;\n"
"            font-size: 16px;\n"
"            font-weight: 600;\n"
"            transition: background-color 0.3s, transform 0.2s;\n"
"        }\n"
"        button:hover {\n"
"            background-color: #2980b9;\n"
"        }\n"
"        button:active {\n"
"            transform: translateY(1px);\n"
"        }\n"
"        button.secondary {\n"
"            background: #95a5a6;\n"
"            margin-left: 8px;\n"
"        }\n"
"        button.secondary:hover {\n"
"            background: #7f8c8d;\n"
"        }\n"
"        .button-group {\n"
"            display: flex;\n"
"            justify-content: space-between;\n"
"            margin-top: 20px;\n"
"        }\n"
"        .network-list {\n"
"            margin-top: 20px;\n"
"            max-height: 300px;\n"
"            overflow-y: auto;\n"
"            border: 1px solid #ddd;\n"
"            border-radius: 4px;\n"
"        }\n"
"        .network-item {\n"
"            padding: 14px;\n"
"            border-bottom: 1px solid #eee;\n"
"            cursor: pointer;\n"
"            transition: background 0.2s;\n"
"            display: flex;\n"
"            justify-content: space-between;\n"
"            align-items: center;\n"
"        }\n"
"        .network-item:hover {\n"
"            background: #ecf0f1;\n"
"        }\n"
"        .network-item:last-child {\n"
"            border-bottom: none;\n"
"        }\n"
"        .signal-strength {\n"
"            color: #7f8c8d;\n"
"        }\n"
"        .lock-icon:after {\n"
"            content: '🔒';\n"
"            margin-left: 6px;\n"
"            font-size: 12px;\n"
"            vertical-align: middle;\n"
"        }\n"
"        .status {\n"
"            font-weight: 600;\n"
"            margin: 0;\n"
"            padding: 4px 8px;\n"
"            border-radius: 3px;\n"
"            display: inline-block;\n"
"        }\n"
"        .status-connected {\n"
"            background: var(--success-color);\n"
"            color: white;\n"
"        }\n"
"        .status-connecting {\n"
"            background: var(--warning-color);\n"
"            color: white;\n"
"        }\n"
"        .status-failed {\n"
"            background: var(--danger-color);\n"
"            color: white;\n"
"        }\n"
"        .status-ap {\n"
"            background: var(--accent-color);\n"
"            color: white;\n"
"        }\n"
"        .current-info {\n"
"            background: #ecf0f1;\n"
"            border-radius: 4px;\n"
"            padding: 15px;\n"
"            margin-bottom: 25px;\n"
"        }\n"
"        .current-info div {\n"
"            margin-bottom: 8px;\n"
"        }\n"
"        .current-info div:last-child {\n"
"            margin-bottom: 0;\n"
"        }\n"
"        .hidden {\n"
"            display: none !important;\n"
"        }\n"
"        .alert {\n"
"            background: #f8d7da;\n"
"            color: #721c24;\n"
"            padding: 12px 15px;\n"
"            border-radius: 4px;\n"
"            margin: 15px 0;\n"
"            border-left: 4px solid #e74c3c;\n"
"        }\n"
"        .success {\n"
"            background: #d4edda;\n"
"            color: #155724;\n"
"            padding: 12px 15px;\n"
"            border-radius: 4px;\n"
"            margin: 15px 0;\n"
"            border-left: 4px solid #2ecc71;\n"
"        }\n"
"        .spinner {\n"
"            display: inline-block;\n"
"            width: 20px;\n"
"            height: 20px;\n"
"            border: 3px solid rgba(52, 152, 219, 0.3);\n"
"            border-radius: 50%;\n"
"            border-top-color: var(--accent-color);\n"
"            animation: spin 1s linear infinite;\n"
"            margin-right: 10px;\n"
"            vertical-align: middle;\n"
"        }\n"
"        #scanning-info {\n"
"            text-align: center;\n"
"            padding: 15px;\n"
"            color: #7f8c8d;\n"
"        }\n"
"        #status-check-container {\n"
"            margin-top: 15px;\n"
"            text-align: center;\n"
"        }\n"
"        @keyframes spin { to { transform: rotate(360deg); } }\n"
"    </style>\n"
"</head>\n"
"<body>\n"
"    <div class=\"container\">\n"
"        <h1>ESP32 WiFi Setup</h1>\n"
"        \n"
"        <div class=\"current-info\">\n"
"            <div><strong>Device:</strong> {{DEVICE_NAME}}</div>\n"
"            <div><strong>Current SSID:</strong> {{CURRENT_SSID}}</div>\n"
"            <div><strong>Status:</strong> <span id=\"connection-status\" class=\"status\">{{CONNECTION_STATUS}}</span></div>\n"
"        </div>\n"
"        \n"
"        <form id=\"wifi-form\" onsubmit=\"saveConfiguration(event)\">\n"
"            <label for=\"ssid\">WiFi Network:</label>\n"
"            <input type=\"text\" id=\"ssid\" name=\"ssid\" autocomplete=\"off\">\n"
"            \n"
"            <label for=\"password\">Password:</label>\n"
"            <div class=\"password-field\">\n"
"                <input type=\"password\" id=\"password\" name=\"password\">\n"
"                <span class=\"password-toggle\" onclick=\"togglePassword()\">Show</span>\n"
"            </div>\n"
"            \n"
"            <div class=\"button-group\">\n"
"                <button type=\"submit\">Connect</button>\n"
"                <div>\n"
"                    <button id=\"scan-button\" type=\"button\" class=\"secondary\" onclick=\"startScan(event)\">Scan Again</button>\n"
"                    <button type=\"button\" class=\"secondary\" onclick=\"forgetNetwork()\">Forget Network</button>\n"
"                </div>\n"
"            </div>\n"
"        </form>\n"
"        \n"
"        <div id=\"alert-message\" class=\"alert hidden\"></div>\n"
"        <div id=\"success-message\" class=\"success hidden\"></div>\n"
"        \n"
"        <div id=\"status-check-container\" class=\"hidden\">\n"
"            <p><span class=\"spinner\"></span> Checking connection status...</p>\n"
"            <button id=\"check-status-button\" type=\"button\" onclick=\"checkConnectionStatus()\">Check Status</button>\n"
"        </div>\n"
"        \n"
"        <div id=\"scanning-info\" class=\"hidden\">\n"
"            <p><span class=\"spinner\"></span> Scanning for networks...</p>\n"
"        </div>\n"
"        \n"
"        <div class=\"network-list\" id=\"network-list\"></div>\n"
"    </div>\n"
"    \n"
"    <script>\n"
"        // Use current SSID value from template\n"
"        const currentSsid = '{{CURRENT_SSID}}' !== 'Not configured' ? '{{CURRENT_SSID}}' : '';\n"
"        const isConnected = currentSsid && currentSsid !== 'Not configured';\n"
"        \n"
"        // Set initial status styling\n"
"        document.addEventListener('DOMContentLoaded', function() {\n"
"            updateStatusStyles('{{CONNECTION_STATUS}}');\n"
"            startScan();\n"
"        });\n"
"        \n"
"        function updateStatusStyles(status) {\n"
"            const statusEl = document.getElementById('connection-status');\n"
"            \n"
"            // Remove all status classes\n"
"            statusEl.classList.remove('status-connected', 'status-connecting', 'status-failed', 'status-ap');\n"
"            \n"
"            // Add appropriate class based on status\n"
"            if (status === 'Connected') {\n"
"                statusEl.classList.add('status-connected');\n"
"            } else if (status === 'Connecting...') {\n"
"                statusEl.classList.add('status-connecting');\n"
"            } else if (status === 'Connection failed') {\n"
"                statusEl.classList.add('status-failed');\n"
"            } else if (status === 'Access Point Mode') {\n"
"                statusEl.classList.add('status-ap');\n"
"            }\n"
"        }\n"
"        \n"
"        function togglePassword() {\n"
"            const passwordInput = document.getElementById('password');\n"
"            const toggleButton = document.querySelector('.password-toggle');\n"
"            \n"
"            if (passwordInput.type === 'password') {\n"
"                passwordInput.type = 'text';\n"
"                toggleButton.textContent = 'Hide';\n"
"            } else {\n"
"                passwordInput.type = 'password';\n"
"                toggleButton.textContent = 'Show';\n"
"            }\n"
"        }\n"
"        \n"
"        function startScan(event) {\n"
"            if (event) event.preventDefault();\n"
"            \n"
"            document.getElementById('scanning-info').style.display = 'block';\n"
"            document.getElementById('network-list').innerHTML = '';\n"
"            \n"
"            fetch('/scan')\n"
"                .then(response => {\n"
"                    if (!response.ok) {\n"
"                        throw new Error('Network scan failed');\n"
"                    }\n"
"                    return response.json();\n"
"                })\n"
"                .then(networks => {\n"
"                    displayNetworks(networks);\n"
"                })\n"
"                .catch(error => {\n"
"                    showAlert('Failed to scan for networks: ' + error.message);\n"
"                    document.getElementById('scanning-info').style.display = 'none';\n"
"                });\n"
"        }\n"
"        \n"
"        function displayNetworks(networks) {\n"
"            const networkList = document.getElementById('network-list');\n"
"            networkList.innerHTML = '';\n"
"            \n"
"            if (networks.length === 0) {\n"
"                networkList.innerHTML = '<div class=\"network-item\">No networks found</div>';\n"
"                networkList.classList.remove('hidden');\n"
"                document.getElementById('scanning-info').style.display = 'none';\n"
"                return;\n"
"            }\n"
"            \n"
"            // Sort by signal strength\n"
"            networks.sort((a, b) => b.rssi - a.rssi);\n"
"            \n"
"            // Create network items\n"
"            networks.forEach(network => {\n"
"                const item = document.createElement('div');\n"
"                item.className = 'network-item';\n"
"                \n"
"                // Get signal strength icon\n"
"                let signalStrength = '';\n"
"                if (network.rssi > -50) signalStrength = '●●●●';\n"
"                else if (network.rssi > -60) signalStrength = '●●●○';\n"
"                else if (network.rssi > -70) signalStrength = '●●○○';\n"
"                else signalStrength = '●○○○';\n"
"                \n"
"                const isEncrypted = network.auth > 0;\n"
"                \n"
"                item.innerHTML = \n"
"                    '<div>' + \n"
"                        escapeHtml(network.ssid) + \n"
"                        (isEncrypted ? '<span class=\"lock-icon\"></span>' : '') + \n"
"                    '</div>' +\n"
"                    '<span class=\"signal-strength\">' + signalStrength + '</span>';\n"
"                \n"
"                item.addEventListener('click', function() {\n"
"                    document.getElementById('ssid').value = network.ssid;\n"
"                    document.getElementById('password').focus();\n"
"                });\n"
"                \n"
"                networkList.appendChild(item);\n"
"            });\n"
"            \n"
"            networkList.classList.remove('hidden');\n"
"            document.getElementById('scanning-info').style.display = 'none';\n"
"        }\n"
"        \n"
"        function saveConfiguration(event) {\n"
"            event.preventDefault();\n"
"            \n"
"            const ssid = document.getElementById('ssid').value.trim();\n"
"            const password = document.getElementById('password').value;\n"
"            \n"
"            if (!ssid) {\n"
"                showAlert('Please enter a WiFi name (SSID)');\n"
"                return;\n"
"            }\n"
"            \n"
"            // Show connecting message\n"
"            showSuccess('Connecting to network...');\n"
"            \n"
"            // Send connection request\n"
"            fetch('/connect', {\n"
"                method: 'POST',\n"
"                headers: {\n"
"                    'Content-Type': 'application/x-www-form-urlencoded',\n"
"                },\n"
"                body: 'ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(password),\n"
"            })\n"
"                .then(response => {\n"
"                    if (!response.ok) {\n"
"                        throw new Error('Connection failed');\n"
"                    }\n"
"                    return response.text();\n"
"                })\n"
"                .then(text => {\n"
"                    // Update status and show status check option\n"
"                    updateStatusStyles('Connecting...');\n"
"                    document.getElementById('connection-status').textContent = 'Connecting...';\n"
"                    \n"
"                    showSuccess('WiFi credentials saved! The device is now attempting to connect while ' +\n"
"                              'keeping the access point available. Use the Check Status button to ' +\n"
"                              'see if the connection was successful.');\n"
"                    \n"
"                    // Show status check container\n"
"                    document.getElementById('status-check-container').classList.remove('hidden');\n"
"                    \n"
"                    // Set up periodic status check\n"
"                    startStatusCheck();\n"
"                })\n"
"                .catch(error => {\n"
"                    showAlert('Failed to connect: ' + error.message);\n"
"                });\n"
"        }\n"
"        \n"
"        function startStatusCheck() {\n"
"            // Check immediately\n"
"            checkConnectionStatus();\n"
"            \n"
"            // Set up interval to check every 3 seconds, for up to 30 seconds\n"
"            let count = 0;\n"
"            const maxCount = 10; // 10 * 3 seconds = 30 seconds\n"
"            \n"
"            const interval = setInterval(() => {\n"
"                count++;\n"
"                if (count >= maxCount) {\n"
"                    clearInterval(interval);\n"
"                    return;\n"
"                }\n"
"                \n"
"                checkConnectionStatus(false)\n"
"                    .then(status => {\n"
"                        if (status === 'Connected') {\n"
"                            clearInterval(interval);\n"
"                        }\n"
"                    })\n"
"                    .catch(() => {});\n"
"            }, 3000);\n"
"        }\n"
"        \n"
"        function checkConnectionStatus(showMessages = true) {\n"
"            if (showMessages) {\n"
"                document.getElementById('status-check-container').querySelector('p').classList.remove('hidden');\n"
"                document.getElementById('check-status-button').disabled = true;\n"
"            }\n"
"            \n"
"            return fetch('/status')\n"
"                .then(response => {\n"
"                    if (!response.ok) {\n"
"                        throw new Error('Failed to get status');\n"
"                    }\n"
"                    return response.json();\n"
"                })\n"
"                .then(data => {\n"
"                    const status = data.status;\n"
"                    \n"
"                    // Update the UI\n"
"                    document.getElementById('connection-status').textContent = status;\n"
"                    updateStatusStyles(status);\n"
"                    \n"
"                    if (showMessages) {\n"
"                        // Show appropriate message based on status\n"
"                        if (status === 'Connected') {\n"
"                            showSuccess('Successfully connected to the WiFi network! You can now access your device at: ' + data.ip);\n"
"                            document.getElementById('status-check-container').classList.add('hidden');\n"
"                        } else if (status === 'Connection failed') {\n"
"                            showAlert('Failed to connect to the WiFi network. Please check your credentials and try again.');\n"
"                        } else {\n"
"                            showSuccess('Still trying to connect. Please check again in a moment.');\n"
"                        }\n"
"                        \n"
"                        document.getElementById('status-check-container').querySelector('p').classList.add('hidden');\n"
"                        document.getElementById('check-status-button').disabled = false;\n"
"                    }\n"
"                    \n"
"                    return status;\n"
"                })\n"
"                .catch(error => {\n"
"                    if (showMessages) {\n"
"                        showAlert('Failed to check connection status: ' + error.message);\n"
"                        document.getElementById('status-check-container').querySelector('p').classList.add('hidden');\n"
"                        document.getElementById('check-status-button').disabled = false;\n"
"                    }\n"
"                    return Promise.reject(error);\n"
"                });\n"
"        }\n"
"        \n"
"        function forgetNetwork() {\n"
"            if (!confirm('Are you sure you want to forget the saved WiFi network?')) {\n"
"                return;\n"
"            }\n"
"            \n"
"            fetch('/reset', {\n"
"                method: 'POST'\n"
"            })\n"
"                .then(response => {\n"
"                    if (!response.ok) {\n"
"                        throw new Error('Failed to reset network settings');\n"
"                    }\n"
"                    return response.text();\n"
"                })\n"
"                .then(text => {\n"
"                    showSuccess('Network settings have been reset. The device will restart in access point mode.');\n"
"                    \n"
"                    // Clear form\n"
"                    document.getElementById('ssid').value = '';\n"
"                    document.getElementById('password').value = '';\n"
"                    \n"
"                    // Update status\n"
"                    document.getElementById('connection-status').textContent = 'Access Point Mode';\n"
"                    updateStatusStyles('Access Point Mode');\n"
"                })\n"
"                .catch(error => {\n"
"                    showAlert('Failed to reset network settings: ' + error.message);\n"
"                });\n"
"        }\n"
"        \n"
"        function showAlert(message) {\n"
"            const alert = document.getElementById('alert-message');\n"
"            alert.textContent = message;\n"
"            alert.style.display = 'block';\n"
"            \n"
"            // Hide success message if it's visible\n"
"            document.getElementById('success-message').style.display = 'none';\n"
"            \n"
"            // Auto-hide after 5 seconds\n"
"            setTimeout(() => {\n"
"                alert.style.display = 'none';\n"
"            }, 5000);\n"
"        }\n"
"        \n"
"        function showSuccess(message) {\n"
"            const success = document.getElementById('success-message');\n"
"            success.textContent = message;\n"
"            success.style.display = 'block';\n"
"            \n"
"            // Hide alert message if it's visible\n"
"            document.getElementById('alert-message').style.display = 'none';\n"
"        }\n"
"        \n"
"        function escapeHtml(unsafe) {\n"
"            return unsafe\n"
"                .replace(/&/g, \"&amp;\")\n"
"                .replace(/</g, \"&lt;\")\n"
"                .replace(/>/g, \"&gt;\")\n"
"                .replace(/\"/g, \"&quot;\")\n"
"                .replace(/'/g, \"&#039;\");\n"
"        }\n"
"    </script>\n"
"</body>\n"
"</html>";

// Simple redirect page for captive portal
const char html_redirect[] = 
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"    <meta http-equiv=\"refresh\" content=\"0;url=/\">\n"
"</head>\n"
"<body>\n"
"    <p>Redirecting to WiFi setup...</p>\n"
"</body>\n"
"</html>";

// Response for Apple CNA (Captive Network Assistant)
const char html_apple_cna[] = 
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"    <title>Success</title>\n"
"    <meta http-equiv=\"refresh\" content=\"0;url=/\">\n"
"</head>\n"
"<body>\n"
"    <h1>Success</h1>\n"
"    <p>You are connected to the ESP32 WiFi setup portal.</p>\n"
"    <p>Click <a href=\"/\">here</a> if you are not redirected automatically.</p>\n"
"</body>\n"
"</html>";
