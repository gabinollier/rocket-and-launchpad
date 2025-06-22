export const api = {
    async fetchRocketState() {
        try {
            const response = await fetch("/api/get-rocket-state");
            if (!response.ok) {
                console.error("Failed to fetch rocket state:", response.statusText);
                return "DISCONNECTED";
            }
            const data = await response.json();
            return data["rocket-state"];
        } catch (error) {
            console.error("Error fetching rocket state:", error);
            return "UNKNOWN";
        }
    },

    async fetchLaunchpadState() {
        try {
            const response = await fetch("/api/get-launchpad-state");
            if (!response.ok) {
                console.error("Failed to fetch launchpad state:", response.statusText);
                return "UNKNOWN";
            }
            const data = await response.json();
            return data["launchpad-state"];
        } catch (error) {
            console.error("Error fetching launchpad state:", error);
            return "UNKNOWN";
        }
    },

    async fetchPressure() {
        try {
            const response = await fetch("/api/get-pressure");
            if (!response.ok) {
                console.error("Failed to fetch pressure:", response.statusText);
                return null;
            }
            const data = await response.json();
            return data["pressure"];
        } catch (error) {
            console.error("Error fetching pressure:", error);
            return null;
        }
    },

    async fetchWaterVolume() {
        try {
            const response = await fetch("/api/get-water-volume");
            if (!response.ok) {
                console.error("Failed to fetch water volume:", response.statusText);
                return null;
            }
            const data = await response.json();
            return data["water-volume"];
        } catch (error) {
            console.error("Error fetching water volume:", error);
            return null;
        }
    },

    async fetchAllFlightTimestamps() {
        try {
            const response = await fetch("/api/get-all-flight-timestamps");
            if (!response.ok) {
                console.error("Failed to fetch flight timestamps:", response.statusText);
                return [];
            }
            const data = await response.json();
            return Array.isArray(data) ? data : [];
        } catch (error) {
            console.error("Error fetching flight timestamps:", error);
            return [];
        }
    },

    async fetchFlightData(timestamp) {
        try {
            const response = await fetch(`/api/get-flight-data?timestamp=${timestamp}`);
            if (!response.ok) {
                console.error("Failed to fetch flight data:", response.statusText);
                return null;
            }
            const data = await response.json();
            return data;
        } catch (error) {
            console.error("Error fetching flight data:", error);
            return null;
        }
    },

    async startFilling(waterVolume, pressure) {
        const epochTime = Math.floor(Date.now() / 1000);
        return await fetch(`/api/start-filling?water-volume=${waterVolume}&pressure=${pressure}&epoch-time=${epochTime}`, {
            method: 'POST'
        });
    },

    async launch()
    {
        return await fetch("/api/launch", {
            method: 'POST'
        });
    },    
    async abort()
    {
        return await fetch("/api/abort", {
            method: 'POST'
        });
    },

    async closeFairing() {
        return await fetch("/api/close-fairing", {
            method: 'POST'
        });
    },

    async openFairing() {
        return await fetch("/api/open-fairing", {
            method: 'POST'
        });
    },

    async skipWaterFilling() {
        return await fetch("/api/skip-water-filling", {
            method: 'POST'
        });
    },

    async skipPressurizing() {
        return await fetch("/api/skip-pressurizing", {
            method: 'POST'
        });
    }
};
