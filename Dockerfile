# ==========================================
# STAGE 1: The Builder
# ==========================================
# Use an official GCC image that has the C++20 compiler and Make installed
FROM gcc:12-bookworm AS builder

RUN apt-get update && apt-get install -y libpqxx-dev

# Set the working directory inside the container
WORKDIR /app

# Copy the Makefile and source code directories into the container
COPY Makefile ./
COPY include/ ./include/
COPY src/ ./src/

# Run the Makefile to compile the C++ code
RUN make

# ==========================================
# STAGE 2: The Runner (Production Image)
# ==========================================
# Use a highly stripped-down version of Debian Linux
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y libpqxx-6.4 && rm -rf /var/lib/apt/lists/*

# Set the working directory for the final image
WORKDIR /app

# Copy only the compiled binary from the builder stage
COPY --from=builder /app/server ./server

# copy the public directory for the static files (HTML/CSS)
COPY public/ ./public/

# Set default Environment Variables
ENV PORT=8080
ENV THREADS=4

EXPOSE 8080

# The command that runs when the container starts
CMD ["./server"]