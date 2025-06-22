import { api } from "../modules/api.js";


export const onPageLoad = (args) => {
    if (args && args.timestamp) {
        showFlightData(args.timestamp);
        setupDownloadButton(args.timestamp);
    }
    else {
        showTimestampRequired();
    }
};



function setupDownloadButton(timestamp) {
    const downloadBtn = document.getElementById('download-btn');
    if (downloadBtn) {
        downloadBtn.textContent = 'Télécharger CSV'; 
        downloadBtn.classList.remove('hidden');
        downloadBtn.onclick = () => {
            const link = document.createElement('a');
            link.href = `/api/download-flight-csv?timestamp=${timestamp}`;
            link.download = `flight_${timestamp}.csv`;
            document.body.appendChild(link);
            link.click();
            document.body.removeChild(link);
        };
    }
}

function showTimestampRequired() {
    const flightDataContainer = document.querySelector('.bg-white.p-6.rounded-lg.shadow-md');
    if (flightDataContainer) {
        flightDataContainer.textContent = 'Timestamp is required to view flight data.';
    }
}

async function showFlightData(timestamp) {
    const flightMetricsContainer = document.getElementById('flight-metrics');
    if (!flightMetricsContainer) return;

    const date = new Date(parseInt(timestamp) * 1000); // Multiply by 1000 to convert seconds to milliseconds
    const formattedDate = date.toLocaleString();

    document.getElementById("title").innerText = `Données de vol du ${formattedDate}`;

    // Show loading message
    flightMetricsContainer.innerHTML = `<p>Chargement des données de vol...</p>`;

    try {
        // Fetch data
        const apiResponse = await api.fetchFlightData(timestamp);

        console.log("API Response:", apiResponse);

        if (!apiResponse || !apiResponse.flightData || !Array.isArray(apiResponse.flightData) || apiResponse.flightData.length < 2) { 
            flightMetricsContainer.innerHTML = `<p>Not enough flight data to calculate velocities (need at least two data points in flightData array).</p>`;
            return;
        }

        const actualFlightData = apiResponse.flightData;
        const targetWaterVolume = apiResponse.targetWaterVolume !== undefined ? apiResponse.targetWaterVolume : 'N/A';
        const targetPressure = apiResponse.targetPressure !== undefined ? apiResponse.targetPressure : 'N/A';

        var flightDuration = (Number(actualFlightData[actualFlightData.length-1].timestamp) - Number(actualFlightData[0].timestamp)) / 1000;

        const velocities = [];
        var maxAltitude = actualFlightData[0].relativeAltitude;
        var maxVelocity = 0;

        for (let i = 1; i < actualFlightData.length; i++) {
            const previousData = actualFlightData[i-1];
            const data = actualFlightData[i];

            if (data.relativeAltitude > maxAltitude) {
                maxAltitude = data.relativeAltitude;
            }

            const dt = (Number(data.timestamp) - Number(previousData.timestamp)) / 1000;
            
            if (dt === 0) { // Avoid division by zero
                velocities.push(0); 
            } else {
                const velocity = (data.relativeAltitude - previousData.relativeAltitude) / dt;
                velocities.push(velocity);
                if (maxVelocity < velocity) {
                    maxVelocity = velocity;
                }
            }
        }        
        
        const smoothedVelocities = [];
        const windowSize = 4; 
        for (let i = 0; i < velocities.length; i++) {
            let sum = 0;
            let count = 0;
            for (let j = Math.max(0, i - windowSize + 1); j <= i; j++) {
                sum += velocities[j];
                count++;
            }
            smoothedVelocities.push(sum / count);
        }

        // Display flight metrics
        flightMetricsContainer.innerHTML = `
            <div class="grid grid-cols-1 md:grid-cols-5 gap-6 text-center">
                <div class="bg-white p-4 rounded-lg shadow">
                    <div class="flex items-center justify-center mb-2">
                        <svg class="w-8 h-8 text-blue-500 mr-2" fill="none" stroke="currentColor" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 8v4l3 3m6-3a9 9 0 11-18 0 9 9 0 0118 0z"></path></svg>
                        <h3 class="text-lg font-semibold text-gray-700">Durée</h3>
                    </div>
                    <p class="text-2xl font-bold text-gray-900">${flightDuration.toFixed(2)} s</p>
                </div>
                <div class="bg-white p-4 rounded-lg shadow">
                    <div class="flex items-center justify-center mb-2">
                        <svg class="w-8 h-8 text-green-500 mr-2" fill="none" stroke="currentColor" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M5 10l7-7m0 0l7 7m-7-7v18"></path></svg>
                        <h3 class="text-lg font-semibold text-gray-700">Altitude Max.</h3>
                    </div>
                    <p class="text-2xl font-bold text-gray-900">${maxAltitude.toFixed(2)} m</p>
                </div>
                <div class="bg-white p-4 rounded-lg shadow">
                    <div class="flex items-center justify-center mb-2">
                        <svg class="w-8 h-8 text-red-500 mr-2" fill="none" stroke="currentColor" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 10V3L4 14h7v7l9-11h-7z"></path></svg>
                        <h3 class="text-lg font-semibold text-gray-700">Vitesse Max.</h3>
                    </div>
                    <p class="text-2xl font-bold text-gray-900">${maxVelocity.toFixed(2)} m/s</p>
                </div>
                <div class="bg-white p-4 rounded-lg shadow">
                    <div class="flex items-center justify-center mb-2">
                        <svg class="w-8 h-8 text-purple-500 mr-2" fill="none" stroke="currentColor" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 16H6a2 2 0 01-2-2V6a2 2 0 012-2h8a2 2 0 012 2v2m-6 12h8a2 2 0 002-2v-8a2 2 0 00-2-2h-8a2 2 0 00-2 2v8a2 2 0 002 2z"></path></svg>
                        <h3 class="text-lg font-semibold text-gray-700">Vol. Eau Cible</h3>
                    </div>
                    <p class="text-2xl font-bold text-gray-900">${typeof targetWaterVolume === 'number' ? targetWaterVolume.toFixed(2) + ' L' : targetWaterVolume}</p>
                </div>
                <div class="bg-white p-4 rounded-lg shadow">
                    <div class="flex items-center justify-center mb-2">
                         <svg class="w-8 h-8 text-yellow-500 mr-2" fill="none" stroke="currentColor" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 6V4m0 2a2 2 0 100 4m0-4a2 2 0 110 4m-6 8a2 2 0 100-4m0 4a2 2 0 110-4m0 4v2m0-6V4m6 6v10m6-2a2 2 0 100-4m0 4a2 2 0 110-4m0 4v2m0-6V4"></path></svg>
                        <h3 class="text-lg font-semibold text-gray-700">Pression Cible</h3>
                    </div>
                    <p class="text-2xl font-bold text-gray-900">${typeof targetPressure === 'number' ? targetPressure.toFixed(2) + ' bar' : targetPressure}</p>
                </div>
            </div>
        `;        
        
        // Charts
        createAltitudeChart(actualFlightData);
        createVelocityChart(actualFlightData, smoothedVelocities);
        createAccelerationChart(actualFlightData);
        createGyroChart(actualFlightData);
        createTemperatureChart(actualFlightData);
        createPressureChart(actualFlightData);

    } catch (error) {
        flightMetricsContainer.innerHTML = `<p style="color: red;">Erreur lors du chargement ou du traitement des données de vol: ${error.message}</p>`; 
    }
}



function createAltitudeChart(flightData) {
    const ctx = document.getElementById('altitude-chart');
    if (!ctx) return;

    // Prepare data for Chart.js
    const labels = [];
    const altitudeData = [];
    const startTime = Number(flightData[0].timestamp);

    flightData.forEach(point => {
        const timeOffset = (Number(point.timestamp) - startTime) / 1000; // Convert to seconds
        labels.push(timeOffset.toFixed(2));
        altitudeData.push(point.relativeAltitude);
    });

    // Destroy existing chart if it exists
    if (window.altitudeChart instanceof Chart) {
        window.altitudeChart.destroy();
    }

    // Create new chart
    window.altitudeChart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: labels,
            datasets: [{
                label: 'Altitude (m)',
                data: altitudeData,
                borderColor: 'rgb(59, 130, 246)',
                backgroundColor: 'rgba(59, 130, 246, 0.1)',
                borderWidth: 2,
                fill: true,
                tension: 0.1
            }]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            scales: {
                x: {
                    display: true,
                    title: {
                        display: true,
                        text: 'Temps (s)'
                    }
                },
                y: {
                    display: true,
                    title: {
                        display: true,
                        text: 'Altitude (m)'
                    },
                    beginAtZero: true
                }
            },
            plugins: {
                legend: {
                    display: false,
                },
            }
        }
    });
}

function createVelocityChart(flightData, velocities) {
    const ctx = document.getElementById('velocity-chart');
    if (!ctx) return;

    // Prepare data for Chart.js
    const labels = [];
    const velocityData = [];
    const startTime = Number(flightData[0].timestamp);

    // Start from index 1 since velocities array has one less element than flightData
    for (let i = 1; i < flightData.length; i++) {
        const timeOffset = (Number(flightData[i].timestamp) - startTime) / 1000; // Convert to seconds
        labels.push(timeOffset.toFixed(2));
        velocityData.push(velocities[i - 1]); // velocities array is offset by 1
    }

    // Destroy existing chart if it exists
    if (window.velocityChart instanceof Chart) {
        window.velocityChart.destroy();
    }

    // Create new chart
    window.velocityChart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: labels,
            datasets: [{
                label: 'Vitesse (m/s)',
                data: velocityData,
                borderColor: 'rgb(239, 68, 68)',
                backgroundColor: 'rgba(239, 68, 68, 0.1)',
                borderWidth: 2,
                fill: true,
                tension: 0.1
            }]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            scales: {
                x: {
                    display: true,
                    title: {
                        display: true,
                        text: 'Temps (s)'
                    }
                },
                y: {
                    display: true,
                    title: {
                        display: true,
                        text: 'Vitesse verticale (m/s)'
                    }
                }
            },
            plugins: {
                legend: {
                    display: false,
                },
            }
        }
    });
}

function createAccelerationChart(flightData) {
    const ctx = document.getElementById('acceleration-chart');
    if (!ctx) return;

    const labels = [];
    const accelMagnitudeData = [];
    const accelXData = [];
    const accelYData = [];
    const accelZData = [];
    const startTime = Number(flightData[0].timestamp);

    flightData.forEach(point => {
        const timeOffset = (Number(point.timestamp) - startTime) / 1000; // Convert to seconds
        labels.push(timeOffset.toFixed(2));
        
        const accelX = point.accelX;
        const accelY = point.accelY;
        const accelZ = point.accelZ;
        
        accelXData.push(accelX);
        accelYData.push(accelY);
        accelZData.push(accelZ);
        accelMagnitudeData.push(Math.sqrt(accelX**2 + accelY**2 + accelZ**2));
    });

    if (window.accelerationChart instanceof Chart) {
        window.accelerationChart.destroy();
    }

    window.accelerationChart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: labels,
            datasets: [
                {
                    label: 'Norme (m/s²)',
                    data: accelMagnitudeData,
                    borderColor: 'rgb(255, 217, 27)',
                    backgroundColor: 'rgba(255, 217, 27, 0.1)',
                    borderWidth: 2,
                    fill: false,
                    tension: 0.1
                },
                {
                    label: 'Accel X (m/s²)',
                    data: accelXData,
                    borderColor: 'rgba(255, 21, 68, 0.3)',
                    backgroundColor: 'rgba(255, 21, 68, 0.1)',
                    borderWidth: 1,
                    fill: false,
                    tension: 0.1,
                    borderDash: [2, 2] // Dashed line for less visibility
                },
                {
                    label: 'Accel Y (m/s²)',
                    data: accelYData,
                    borderColor: 'rgba(40, 192, 70, 0.3)',
                    backgroundColor: 'rgba(40, 192, 70, 0.1)',
                    borderWidth: 1,
                    fill: false,
                    tension: 0.1,
                    borderDash: [2, 2] // Dashed line for less visibility
                },
                {
                    label: 'Accel Z (m/s²)',
                    data: accelZData,
                    borderColor: 'rgba(54, 90, 250, 0.3)',
                    backgroundColor: 'rgba(54, 90, 250, 0.1)',
                    borderWidth: 1,
                    fill: false,
                    tension: 0.1,
                    borderDash: [2, 2] // Dashed line for less visibility
                }
            ]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            scales: {
                x: {
                    display: true,
                    title: {
                        display: true,
                        text: 'Temps (s)'
                    }
                },
                y: {
                    display: true,
                    title: {
                        display: true,
                        text: 'Accélération (m/s²)'
                    }
                }
            },
            plugins: {
                legend: {
                    display: true, // Show legend
                },
            }
        }
    });
}

function createGyroChart(flightData) {
    const ctx = document.getElementById('gyro-chart');
    if (!ctx) return;

    const labels = [];
    const gyroXData = [];
    const gyroYData = [];
    const gyroZData = [];
    const startTime = Number(flightData[0].timestamp);

    flightData.forEach(point => {
        const timeOffset = (Number(point.timestamp) - startTime) / 1000;        labels.push(timeOffset.toFixed(2));
        gyroXData.push(point.gyroX);
        gyroYData.push(point.gyroY);
        gyroZData.push(point.gyroZ);
    });

    if (window.gyroChart instanceof Chart) {
        window.gyroChart.destroy();
    }

    window.gyroChart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: labels,
            datasets: [
                {
                    label: 'Gyro X (rad/s)',
                    data: gyroXData,
                    borderColor: 'rgba(255, 21, 68, 0.8)',
                    backgroundColor: 'rgba(255, 21, 68, 0.1)',
                    borderWidth: 1,
                    fill: false,
                    tension: 0.1
                },
                {
                    label: 'Gyro Y (rad/s)',
                    data: gyroYData,
                    borderColor: 'rgba(40, 192, 70, 0.8)',
                    backgroundColor: 'rgba(40, 192, 70, 0.1)',
                    borderWidth: 1,
                    fill: false,
                    tension: 0.1
                },
                {
                    label: 'Gyro Z (rad/s)',
                    data: gyroZData,
                    borderColor: 'rgba(54, 90, 250, 0.8)',
                    backgroundColor: 'rgba(54, 90, 250, 0.1)',
                    borderWidth: 1,
                    fill: false,
                    tension: 0.1
                }
            ]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            scales: {
                x: {
                    display: true,
                    title: {
                        display: true,
                        text: 'Temps (s)'
                    }
                },
                y: {
                    display: true,
                    title: {
                        display: true,
                        text: 'Vitesse angulaire (rad/s)'
                    }
                }
            },
            plugins: {
                legend: {
                    display: true,
                },
            }
        }
    });
}

function createTemperatureChart(flightData) {
    const ctx = document.getElementById('temperature-chart');
    if (!ctx) return;

    const labels = [];
    const temperatureData = [];
    const startTime = Number(flightData[0].timestamp);

    flightData.forEach(point => {
        const timeOffset = (Number(point.timestamp) - startTime) / 1000;        labels.push(timeOffset.toFixed(2));
        temperatureData.push(point.temperature);
    });

    if (window.temperatureChart instanceof Chart) {
        window.temperatureChart.destroy();
    }

    window.temperatureChart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: labels,
            datasets: [{
                label: 'Température (°C)',
                data: temperatureData,
                borderColor: 'rgb(255, 159, 64)',
                backgroundColor: 'rgba(255, 159, 64, 0.1)',
                borderWidth: 2,
                fill: true,
                tension: 0.1
            }]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            scales: {
                x: {
                    display: true,
                    title: {
                        display: true,
                        text: 'Temps (s)'
                    }
                },
                y: {
                    display: true,
                    title: {
                        display: true,
                        text: 'Température (°C)'
                    }
                }
            },
            plugins: {
                legend: {
                    display: false,
                },
            }
        }
    });
}

function createPressureChart(flightData) {
    const ctx = document.getElementById('pressure-chart');
    if (!ctx) return;

    const labels = [];
    const pressureData = [];
    const startTime = Number(flightData[0].timestamp);

    flightData.forEach(point => {
        const timeOffset = (Number(point.timestamp) - startTime) / 1000;        labels.push(timeOffset.toFixed(2));
        pressureData.push(point.pressure);
    });

    if (window.pressureChart instanceof Chart) {
        window.pressureChart.destroy();
    }

    window.pressureChart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: labels,
            datasets: [{
                label: 'Pression (Pa)',
                data: pressureData,
                borderColor: 'rgb(153, 102, 255)',
                backgroundColor: 'rgba(153, 102, 255, 0.1)',
                borderWidth: 2,
                fill: true,
                tension: 0.1
            }]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            scales: {
                x: {
                    display: true,
                    title: {
                        display: true,
                        text: 'Temps (s)'
                    }
                },
                y: {
                    display: true,
                    title: {
                        display: true,
                        text: 'Pression (Pa)'
                    }
                }
            },
            plugins: {
                legend: {
                    display: false,
                },
            }
        }
    });
}