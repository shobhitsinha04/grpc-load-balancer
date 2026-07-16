terraform {
  required_version = ">= 1.5"

  required_providers {
    google = {
      source  = "hashicorp/google"
      version = "~> 5.0"
    }
  }

  # State is local here, which is fine for a single-operator project and wrong
  # for a team: concurrent applies would race and the state file (which holds
  # resource metadata in plaintext) lives only on one laptop. A shared backend
  # is the first thing to add if anyone else touches this.
  #
  # backend "gcs" {
  #   bucket = "REPLACE-tfstate-bucket"
  #   prefix = "grpc-load-balancer"
  # }
}

provider "google" {
  project = var.project_id
  region  = var.region
}
