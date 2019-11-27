library(ggplot2)
library(reshape2)
file <- "first-random-trial.csv"

data <- read.csv(file)

# hist(data$read1, col=rgb(1, 0, 0, 0.5), xlim=c(4000, 5000), ylim=c(0, 1000), breaks=2500)
# hist(data$read2, col=rgb(0, 0, 1, 0.5), breaks=2500, add=T)

frame <- data.frame(cache_miss_read=data$read1, cache_hit_read=data$read2)
# plot(read1.d, xlim=c(4000, 5000))
# polygon(read1.d, col=rgb(1, 0, 0, 0.5))

ggplot(melt(frame), aes(x = value, fill = variable)) + geom_density(alpha=0.5) + xlim(4150, 4750) + labs(title=file)
