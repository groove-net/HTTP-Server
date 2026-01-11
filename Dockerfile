# syntax=docker/dockerfile:1

FROM ubuntu:latest AS runtime

# Define working directory
WORKDIR /app

# Copy program
COPY ./bin/program ./

# Expose port
EXPOSE 3094

ENTRYPOINT ["./program"]
