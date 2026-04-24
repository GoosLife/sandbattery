USE sandbattery;

-- Schema

CREATE TABLE IF NOT EXISTS device (
    product_key VARCHAR(100) NOT NULL PRIMARY KEY,
    device_name VARCHAR(200) NOT NULL
);

CREATE TABLE IF NOT EXISTS settings (
    id INT AUTO_INCREMENT PRIMARY KEY,
    product_key VARCHAR(100) NOT NULL,
    max_sand_temp FLOAT NOT NULL DEFAULT 70.0,
    min_pump_temp FLOAT NOT NULL DEFAULT 50.0,
    pump_interval_seconds INT NOT NULL DEFAULT 300,
    price_limit_dkk FLOAT NOT NULL DEFAULT 1.50,
    auto_heating_enabled TINYINT(1) NOT NULL DEFAULT 1,
    auto_pump_enabled TINYINT(1) NOT NULL DEFAULT 1,
    UNIQUE KEY uq_settings_product_key (product_key)
);

CREATE TABLE IF NOT EXISTS actuator_status (
    id INT AUTO_INCREMENT PRIMARY KEY,
    product_key VARCHAR(100) NOT NULL,
    actuator VARCHAR(50) NOT NULL,
    active TINYINT(1) NOT NULL DEFAULT 0,
    source VARCHAR(50) NOT NULL DEFAULT 'manual',
    last_changed DATETIME NOT NULL,
    UNIQUE KEY uq_actuator_status (product_key, actuator)
);

CREATE TABLE IF NOT EXISTS sensor_measurement (
    id INT AUTO_INCREMENT PRIMARY KEY,
    product_key VARCHAR(100) NOT NULL,
    timestamp DATETIME NOT NULL,
    sand_temp FLOAT NOT NULL,
    water_temp_in FLOAT NOT NULL,
    water_temp_out FLOAT NOT NULL,
    flow_rate FLOAT NOT NULL,
    power_w FLOAT NOT NULL,
    energy_kwh FLOAT NOT NULL,
    status VARCHAR(20) NOT NULL DEFAULT 'OK'
);

CREATE TABLE IF NOT EXISTS event (
    id INT AUTO_INCREMENT PRIMARY KEY,
    product_key VARCHAR(100) NOT NULL,
    type VARCHAR(100) NOT NULL,
    source VARCHAR(100) NOT NULL,
    timestamp DATETIME NOT NULL,
    description TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS alert (
    id INT AUTO_INCREMENT PRIMARY KEY,
    product_key VARCHAR(100) NOT NULL,
    severity VARCHAR(50) NOT NULL,
    type VARCHAR(100) NOT NULL,
    message TEXT NOT NULL,
    timestamp DATETIME NOT NULL,
    acknowledged TINYINT(1) NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS electricity_price (
    id INT AUTO_INCREMENT PRIMARY KEY,
    date VARCHAR(20) NOT NULL,
    area VARCHAR(10) NOT NULL,
    currency VARCHAR(10) NOT NULL DEFAULT 'DKK',
    last_updated DATETIME NOT NULL,
    UNIQUE KEY uq_electricity_price (date, area)
);

CREATE TABLE IF NOT EXISTS price_entry (
    id INT AUTO_INCREMENT PRIMARY KEY,
    electricity_price_id INT NOT NULL,
    hour DATETIME NOT NULL,
    price_dkk_kwh FLOAT NOT NULL,
    CONSTRAINT fk_price_entry_electricity_price
        FOREIGN KEY (electricity_price_id)
        REFERENCES electricity_price(id)
        ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS heartbeat (
    id INT AUTO_INCREMENT PRIMARY KEY,
    product_key VARCHAR(100) NOT NULL,
    timestamp DATETIME NOT NULL,
    uptime_seconds INT NULL
);

-- Seed data

INSERT INTO device (product_key, device_name) VALUES
    ('SB-TEST-001', 'Test Sandbattery Alpha'),
    ('SB-TEST-002', 'Test Sandbattery Beta');

INSERT INTO settings (product_key, max_sand_temp, min_pump_temp, pump_interval_seconds, price_limit_dkk, auto_heating_enabled, auto_pump_enabled) VALUES
    ('SB-TEST-001', 70.0, 50.0, 300, 1.50, 1, 1),
    ('SB-TEST-002', 65.0, 45.0, 600, 2.00, 1, 0);

INSERT INTO actuator_status (product_key, actuator, active, source, last_changed) VALUES
    ('SB-TEST-001', 'heater', 1, 'auto',   '2026-04-22 08:00:00'),
    ('SB-TEST-001', 'pump',   0, 'manual', '2026-04-22 07:45:00'),
    ('SB-TEST-002', 'heater', 0, 'manual', '2026-04-22 06:00:00'),
    ('SB-TEST-002', 'pump',   1, 'auto',   '2026-04-22 08:15:00');

INSERT INTO sensor_measurement (product_key, timestamp, sand_temp, water_temp_in, water_temp_out, flow_rate, power_w, energy_kwh, status) VALUES
    ('SB-TEST-001', '2026-04-22 08:00:00', 62.5, 45.0, 55.0, 2.5, 2000.0, 0.5, 'OK'),
    ('SB-TEST-001', '2026-04-22 08:05:00', 63.1, 46.0, 56.0, 2.5, 2000.0, 0.7, 'OK'),
    ('SB-TEST-001', '2026-04-22 08:10:00', 63.8, 47.0, 57.0, 2.5, 2000.0, 0.9, 'OK'),
    ('SB-TEST-002', '2026-04-22 08:00:00', 58.0, 40.0, 50.0, 3.0, 1800.0, 0.4, 'OK'),
    ('SB-TEST-002', '2026-04-22 08:05:00', 58.7, 41.0, 51.0, 3.0, 1800.0, 0.6, 'OK');

INSERT INTO event (product_key, type, source, timestamp, description) VALUES
    ('SB-TEST-001', 'HEATER_ON',  'auto',   '2026-04-22 08:00:00', 'Heater activated by auto-heating scheduler'),
    ('SB-TEST-001', 'PUMP_OFF',   'manual', '2026-04-22 07:45:00', 'Pump deactivated manually via API'),
    ('SB-TEST-002', 'HEATER_OFF', 'manual', '2026-04-22 06:00:00', 'Heater deactivated manually via API'),
    ('SB-TEST-002', 'PUMP_ON',    'auto',   '2026-04-22 08:15:00', 'Pump activated by auto-pump scheduler');

INSERT INTO alert (product_key, severity, type, message, timestamp, acknowledged) VALUES
    ('SB-TEST-001', 'WARNING', 'HIGH_TEMP',    'Sand temperature approaching max threshold (63.8 / 70.0 °C)', '2026-04-22 08:10:00', 0),
    ('SB-TEST-002', 'INFO',    'PUMP_INACTIVE', 'Pump has been inactive for over 10 minutes',                 '2026-04-22 07:55:00', 1);

INSERT INTO electricity_price (id, date, area, currency, last_updated) VALUES
    (1, '2026-04-22', 'DK1', 'DKK', '2026-04-22 00:01:00'),
    (2, '2026-04-22', 'DK2', 'DKK', '2026-04-22 00:01:00');

INSERT INTO price_entry (electricity_price_id, hour, price_dkk_kwh) VALUES
    (1, '2026-04-22 00:00:00', 0.85),
    (1, '2026-04-22 01:00:00', 0.72),
    (1, '2026-04-22 06:00:00', 1.10),
    (1, '2026-04-22 07:00:00', 1.45),
    (1, '2026-04-22 08:00:00', 1.62),
    (1, '2026-04-22 09:00:00', 1.38),
    (2, '2026-04-22 00:00:00', 0.90),
    (2, '2026-04-22 01:00:00', 0.75),
    (2, '2026-04-22 06:00:00', 1.15),
    (2, '2026-04-22 07:00:00', 1.50),
    (2, '2026-04-22 08:00:00', 1.70),
    (2, '2026-04-22 09:00:00', 1.42);

INSERT INTO heartbeat (product_key, timestamp, uptime_seconds) VALUES
    ('SB-TEST-001', '2026-04-22 08:00:00', 3600),
    ('SB-TEST-001', '2026-04-22 08:05:00', 3900),
    ('SB-TEST-002', '2026-04-22 08:00:00', 7200),
    ('SB-TEST-002', '2026-04-22 08:05:00', 7500);
