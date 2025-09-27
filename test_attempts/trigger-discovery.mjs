#!/usr/bin/env node
/**
 * Trigger kernel discovery in Electron and read the log file
 */

import fs from 'fs';
import { spawn } from 'child_process';

const LOG_FILE = '/tmp/haywire-kernel-discovery.log';

// Function to read and display log
function readLog() {
    if (!fs.existsSync(LOG_FILE)) {
        console.log('Log file not found at', LOG_FILE);
        return null;
    }

    const content = fs.readFileSync(LOG_FILE, 'utf8');
    return content;
}

// Function to monitor log file
function monitorLog() {
    console.log('Monitoring log file:', LOG_FILE);
    console.log('=' .repeat(50));

    let lastSize = 0;

    const checkLog = () => {
        try {
            const stats = fs.statSync(LOG_FILE);
            if (stats.size !== lastSize) {
                const content = fs.readFileSync(LOG_FILE, 'utf8');
                // Only print new content
                const newContent = content.substring(lastSize);
                if (newContent) {
                    process.stdout.write(newContent);
                }
                lastSize = stats.size;
            }
        } catch (err) {
            // File doesn't exist yet
        }
    };

    // Check every 100ms
    const interval = setInterval(checkLog, 100);

    // Stop after 30 seconds
    setTimeout(() => {
        clearInterval(interval);
        console.log('\n' + '=' .repeat(50));
        console.log('Monitoring complete');
    }, 30000);
}

// Main
const command = process.argv[2];

if (command === 'read') {
    const content = readLog();
    if (content) {
        console.log(content);
    }
} else if (command === 'monitor') {
    monitorLog();
} else {
    console.log('Usage:');
    console.log('  node trigger-discovery.mjs read     - Read the current log file');
    console.log('  node trigger-discovery.mjs monitor  - Monitor log file for 30 seconds');
    console.log('');
    console.log('To trigger discovery from Electron:');
    console.log('1. Start Electron app');
    console.log('2. Open memory file');
    console.log('3. Open DevTools console');
    console.log('4. Run: await window.electronAPI.invoke("trigger-kernel-discovery")');
}