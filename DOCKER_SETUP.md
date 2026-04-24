# Sandbattery - Docker Setup

This Docker Compose configuration runs the entire Sandbattery application stack (frontend, backend, and database) with a single command.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Docker Network                          │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  Frontend (Nginx)                                    │  │
│  │  - Port: 3000                                        │  │
│  │  - Proxies /api/* to Backend                         │  │
│  └──────────────────────────────────────────────────────┘  │
│           ↓                                                  │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  Backend (.NET API)                                  │  │
│  │  - Port: 8080                                        │  │
│  │  - Connects to MySQL Database                        │  │
│  └──────────────────────────────────────────────────────┘  │
│           ↓                                                  │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  MySQL Database                                      │  │
│  │  - Port: 3306                                        │  │
│  │  - Database: sandbattery                             │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

## Prerequisites

- Docker Engine 20.10+
- Docker Compose 2.0+

## Quick Start

### 1. Navigate to the Docker directory
```powershell
cd c:\Dev\Docker
```

### 2. Build and start all services
```powershell
docker-compose up --build
```

This will:
- Build the backend Docker image
- Build the frontend Docker image
- Start MySQL database (with initialization from `Db/init.sql`)
- Start the backend API (port 8080)
- Start the frontend (port 3000)

### 3. Access the application
- **Frontend**: http://localhost:3000
- **Backend API**: http://localhost:8080
- **API Documentation**: http://localhost:8080/openapi/v1.json
- **Database**: localhost:3306 (root:rootpassword)

## Configuration

### Environment Variables

The `.env` file contains all configuration variables:

```env
# Database Configuration
MYSQL_ROOT_PASSWORD=rootpassword
MYSQL_DATABASE=sandbattery
MYSQL_USER=sandbattery
MYSQL_PASSWORD=sandbatterypass

# Backend Configuration
BACKEND_PORT=8080

# Frontend Configuration
FRONTEND_PORT=3000

# Database Service Configuration
DB_PORT=3306
```

To modify settings, edit the `.env` file before running `docker-compose up`.

## Common Commands

### Start services
```powershell
docker-compose up
```

### Start in background (detached mode)
```powershell
docker-compose up -d
```

### Stop services
```powershell
docker-compose down
```

### View logs
```powershell
docker-compose logs -f
```

### View logs for specific service
```powershell
docker-compose logs -f backend
docker-compose logs -f frontend
docker-compose logs -f db
```

### Rebuild services
```powershell
docker-compose up --build
```

### Remove everything including volumes (WARNING: deletes database data)
```powershell
docker-compose down -v
```

## Troubleshooting

### Database connection issues
- Ensure the database service is healthy: `docker-compose ps`
- Check database logs: `docker-compose logs db`
- Verify the connection string in the backend configuration

### Frontend can't connect to backend
- Check that the backend service is running: `docker-compose ps`
- Verify backend logs: `docker-compose logs backend`
- Ensure nginx is properly proxying requests (check Frontend logs)

### Port conflicts
If ports 3000, 8080, or 3306 are already in use, modify the `.env` file:
```env
BACKEND_PORT=8081
FRONTEND_PORT=3001
DB_PORT=3307
```

### Docker image rebuild
If you make code changes, rebuild the images:
```powershell
docker-compose up --build
```

## File Structure

```
Docker/
├── .env                          # Environment variables
├── docker-compose.yml            # Main orchestration file
├── Backend/
│   └── sandbattery-backend/
│       ├── docker-compose.yml    # Backend-specific compose (for local dev)
│       ├── Dockerfile
│       └── sandbattery-backend/
│           ├── Program.cs
│           ├── appsettings.json
│           └── ...
├── Frontend/
│   └── frontend/
│       ├── docker-compose.yml    # Frontend-specific compose (for local dev)
│       ├── Dockerfile
│       ├── nginx.conf            # Nginx configuration with API proxy
│       └── ...
└── Db/
    └── init.sql                  # Database initialization script
```

## Network Communication

All services communicate via the `sandbattery-network` Docker network:

- **Frontend → Backend**: `http://backend:8080` (via nginx proxy)
- **Backend → Database**: `Server=db;Port=3306;...` (internal DNS)

## Development vs Production

The current configuration uses:
- **`Production`** environment for the backend
- **`Release`** build configuration for both backend and frontend

For development, run services locally:
```powershell
# Backend
cd Backend/sandbattery-backend/sandbattery-backend
dotnet run

# Frontend
cd Frontend/frontend
npm run dev

# Database (via Docker)
cd Docker
docker-compose up db
```

## Database Initialization

The database automatically initializes using the `Db/init.sql` script. To reset the database:

```powershell
docker-compose down -v
docker-compose up
```

## Notes

- The backend automatically waits for the database to be healthy before starting
- Frontend nginx configuration includes security headers and gzip compression
- All services automatically restart if they crash (unless-stopped policy)
- Database data persists in a Docker volume (`sandbattery-db-data`)
