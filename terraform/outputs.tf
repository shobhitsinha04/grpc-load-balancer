output "cluster_name" {
  description = "Name of the GKE cluster."
  value       = google_container_cluster.primary.name
}

output "cluster_endpoint" {
  description = "Control plane endpoint."
  value       = google_container_cluster.primary.endpoint
  sensitive   = true
}

output "artifact_registry" {
  description = "Docker repository host path to tag images against."
  value       = "${var.region}-docker.pkg.dev/${var.project_id}/${google_artifact_registry_repository.images.repository_id}"
}

output "configure_kubectl" {
  description = "Run this to point kubectl at the cluster."
  value       = "gcloud container clusters get-credentials ${google_container_cluster.primary.name} --region ${var.region} --project ${var.project_id}"
}

output "next_steps" {
  description = "Image push and deploy sequence after apply."
  value       = <<-EOT
    AR=${var.region}-docker.pkg.dev/${var.project_id}/${google_artifact_registry_repository.images.repository_id}

    gcloud auth configure-docker ${var.region}-docker.pkg.dev
    docker build -f docker/Dockerfile --target lb      -t $AR/grpc-lb:dev      .
    docker build -f docker/Dockerfile --target backend -t $AR/grpc-lb-backend:dev .
    docker push $AR/grpc-lb:dev
    docker push $AR/grpc-lb-backend:dev

    # k8s manifests reference ghcr.io by default; retarget them at Artifact Registry:
    cd k8s && kustomize edit set image \
      ghcr.io/shobhitsinha04/grpc-lb=$AR/grpc-lb \
      ghcr.io/shobhitsinha04/grpc-lb-backend=$AR/grpc-lb-backend && cd ..

    kubectl apply -k k8s/
  EOT
}
